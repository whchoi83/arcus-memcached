/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * arcus-memcached - Arcus memory cache server
 * Copyright 2010-2014 NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cluster_config.h"

#define PROTOTYPES 1
#include "rfc1321/md5c.c"
#undef  PROTOTYPES

struct server_item {
    char *hostport;
};

static void server_item_free(struct server_item *servers, int num_servers) {
    int i;
    for (i=0; i<num_servers; i++) {
        free(servers[i].hostport);
    }
}

struct continuum_item {
    uint32_t index;    // server index
    uint32_t point;    // point on the ketama continuum
};

struct cluster_config {
    uint32_t self_id;                    // server index for this memcached
    char     *self_hostport;             // host:port string for this memcached

    int      num_servers;                // number of memcached servers in cluster
    int      num_continuum;              // number of continuum

    struct   server_item *servers;       // server list
    struct   continuum_item *continuum;  // continuum list

    pthread_mutex_t lock;                // cluster lock

    EXTENSION_LOGGER_DESCRIPTOR *logger; // memcached logger
    int      verbose;                    // log level

    bool     is_valid;                   // is this configuration valid?
};

static void hash_md5(const char *key, size_t key_length, unsigned char *result)
{
    MD5_CTX ctx;

    MD5Init(&ctx);
    MD5Update(&ctx, (unsigned char *)key, key_length);
    MD5Final(result, &ctx);
}

static uint32_t hash_ketama(const char *key, size_t key_length)
{
    unsigned char digest[16];

    hash_md5(key, key_length, digest);

    return (uint32_t) ( (digest[3] << 24)
                       |(digest[2] << 16)
                       |(digest[1] << 8)
                       | digest[0]);
}

static int continuum_item_cmp(const void *t1, const void *t2)
{
    const struct continuum_item *ct1 = t1, *ct2 = t2;

    if      (ct1->point == ct2->point)   return  0;
    else if (ct1->point  > ct2->point)   return  1;
    else                                 return -1;
}

#define NUM_OF_HASHES 40
#define NUM_PER_HASH  4

static bool ketama_continuum_generate(struct cluster_config *config,
                                      const struct server_item *servers, size_t num_servers,
                                      struct continuum_item **continuum, size_t *continuum_len)
{
    char host[MAX_SERVER_ITEM_COUNT+10] = "";
    int host_len;
    int pp, hh, ss, nn;
    unsigned char digest[16];

    // 40 hashes, 4 numbers per hash = 160 points per server
    int points_per_server = NUM_OF_HASHES * NUM_PER_HASH;

    *continuum = calloc(points_per_server * num_servers, sizeof(struct continuum_item));

    if (*continuum == NULL) {
        config->logger->log(EXTENSION_LOG_WARNING, NULL, "calloc failed: continuum\n");
        return false;
    }

    for (ss=0, pp=0; ss<num_servers; ss++) {
        for (hh=0; hh<NUM_OF_HASHES; hh++) {
            host_len = snprintf(host, MAX_SERVER_ITEM_COUNT+10, "%s-%u",
                                servers[ss].hostport, hh);
            hash_md5(host, host_len, digest);
            for (nn=0; nn<NUM_PER_HASH; nn++, pp++) {
                (*continuum)[pp].index = ss;
                (*continuum)[pp].point = ((uint32_t) (digest[3 + nn * NUM_PER_HASH] & 0xFF) << 24)
                                       | ((uint32_t) (digest[2 + nn * NUM_PER_HASH] & 0xFF) << 16)
                                       | ((uint32_t) (digest[1 + nn * NUM_PER_HASH] & 0xFF) <<  8)
                                       | (           (digest[0 + nn * NUM_PER_HASH] & 0xFF)      );
            }
        }
    }

    qsort(*continuum, pp, sizeof(struct continuum_item), continuum_item_cmp);

    *continuum_len = pp;

    return true;
}

static bool server_item_populate(struct cluster_config *config,
                                 char **server_list, size_t num_servers, const char *self_hostport,
                                 struct server_item **servers, uint32_t *self_id)
{
    int i;

    assert(*servers == NULL);

    *servers = calloc(num_servers, sizeof(struct server_item));

    if (*servers == NULL) {
        config->logger->log(EXTENSION_LOG_WARNING, NULL, "calloc failed: servers\n");
        return false;
    }

    for (i=0; i<num_servers; i++) {
        char *buf = NULL;
        char *tok = NULL;

        // filter characters after dash(-)
        for (tok=strtok_r(server_list[i], "-", &buf);
             tok;
             tok=strtok_r(NULL, "-", &buf)) {
            char *hostport = strdup(server_list[i]);

            if (hostport == NULL) {
                config->logger->log(EXTENSION_LOG_WARNING, NULL, "invalid server token\n");
                server_item_free(*servers, i);
                free(*servers);
                *servers = NULL;
                return false;
            }

            (*servers)[i].hostport = hostport;

            if (strcmp(self_hostport, hostport) == 0) {
                *self_id = i;
            }

            break;
        }
    }

    return true;
}

struct cluster_config *cluster_config_init(EXTENSION_LOGGER_DESCRIPTOR *logger, int verbose)
{
    struct cluster_config *config;
    int err;

    config = calloc(1, sizeof(struct cluster_config));

    if (config == NULL) {
        logger->log(EXTENSION_LOG_WARNING, NULL, "calloc failed: cluster_config\n");
        return NULL;
    }

    err = pthread_mutex_init(&config->lock, NULL);
    assert(err == 0);

    config->logger = logger;
    config->verbose = verbose;

    config->is_valid = false;

    return config;
}

void cluster_config_free(struct cluster_config *config)
{
    if (config == NULL) {
        return;
    }

    if (config->continuum) {
        free(config->continuum);
        config->continuum = NULL;
    }

    if (config->servers) {
        server_item_free(config->servers, config->num_servers);
        free(config->servers);
        config->servers = NULL;
    }

    pthread_mutex_destroy(&config->lock);
    free(config);
}

uint32_t cluster_config_self_id (struct cluster_config *config)
{
    assert(config);
    return config->self_id;
}

int cluster_config_num_servers(struct cluster_config *config)
{
    assert(config);
    return config->num_servers;
}

int cluster_config_num_continuum(struct cluster_config *config)
{
    assert(config);
    return config->num_continuum;
}

bool cluster_config_is_valid(struct cluster_config *config)
{
    assert(config);
    return config->is_valid;
}

void cluster_config_set_hostport(struct cluster_config *config, const char *hostport, size_t hostport_len)
{
    assert(config);
    assert(hostport);
    assert(hostport_len > 0);
    config->self_hostport = strndup(hostport, hostport_len);
}

bool cluster_config_reconfigure(struct cluster_config *config, char **server_list, size_t num_servers)
{
    assert(config);
    assert(server_list);

    uint32_t self_id = 0;
    size_t num_continuum = 0;
    struct server_item *servers = NULL;
    struct continuum_item *continuum = NULL;

    bool populated, generated;

    populated = server_item_populate(config, server_list, num_servers, config->self_hostport,
                                     &servers, &self_id);
    if (!populated) {
        config->logger->log(EXTENSION_LOG_WARNING, NULL, "reconfiguration failed: server_item_populate\n");
        goto RECONFIG_FAILED;
    }

    generated = ketama_continuum_generate(config, servers, num_servers,
                                          &continuum, &num_continuum);
    if (!generated) {
        config->logger->log(EXTENSION_LOG_WARNING, NULL, "reconfiguration failed: ketama_continuum_generate\n");
        server_item_free(servers, num_servers);
        free(servers);
        servers = NULL;
        goto RECONFIG_FAILED;
    }

    pthread_mutex_lock(&config->lock);

    server_item_free(config->servers, config->num_servers);
    free(config->servers);
    free(config->continuum);

    config->self_id = self_id;
    config->num_servers = num_servers;
    config->servers = servers;
    config->continuum = continuum;
    config->num_continuum = num_continuum;
    config->is_valid = true;

    pthread_mutex_unlock(&config->lock);

    return true;

RECONFIG_FAILED:
    pthread_mutex_lock(&config->lock);
    config->is_valid = false;
    pthread_mutex_unlock(&config->lock);

    return false;
}

bool cluster_config_key_is_mine(struct cluster_config *config, const char *key, size_t nkey,
                                uint32_t *key_id, uint32_t *self_id)
{
    uint32_t server, self;
    uint32_t digest, mid, prev;
    struct continuum_item *beginp, *endp, *midp, *highp, *lowp;

    assert(config);
    assert(config->continuum);

    pthread_mutex_lock(&config->lock);

    // this should not be happened
    if (config->is_valid == false) {
        pthread_mutex_unlock(&config->lock);
        return true;
    }

    self = config->self_id;
    digest = hash_ketama(key, nkey);
    beginp = lowp = config->continuum;
    endp = highp = config->continuum + config->num_continuum;

    while (1) {
        // pick the middle point
        midp = lowp + (highp - lowp) / 2;

        if (midp == endp) {
            // if at the end, rollback to 0th
            server = beginp->index;
            break;
        }

        mid = midp->point;
        prev = (midp == beginp) ? 0 : (midp-1)->point;

        if (digest <= mid && digest > prev) {
            // found the nearest server
            server = midp->index;
            break;
        }

        // adjust the limits
        if (mid < digest)     lowp = midp + 1;
        else                 highp = midp - 1;

        if (lowp > highp) {
            server = beginp->index;
            break;
        }
    }

    if ( key_id)  *key_id = server;
    if (self_id) *self_id = self;

    pthread_mutex_unlock(&config->lock);

    return server == self;
}
