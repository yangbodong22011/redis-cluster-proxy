/*
 * Copyright (C) 2019  Giuseppe Fabio Nicotra <artix2 at gmail dot com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "anet.h"
#include "cluster.h"
#include "zmalloc.h"
#include "logger.h"
#include "config.h"

#define CLUSTER_NODE_KEEPALIVE_INTERVAL 15
#define CLUSTER_PRINT_REPLY_ERROR(n, err) \
    proxyLogErr("Node %s:%d replied with error:\n%s\n", \
                n->ip, n->port, err);

uint16_t crc16(const char *buf, int len);

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static unsigned int clusterKeyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* Check whether reply is NULL or its type is REDIS_REPLY_ERROR. In the
 * latest case, if the 'err' arg is not NULL, it gets allocated with a copy
 * of reply error (it's up to the caller function to free it), elsewhere
 * the error is directly printed. */
static int clusterCheckRedisReply(clusterNode *n, redisReply *r, char **err) {
    int is_err = 0;
    if (!r || (is_err = (r->type == REDIS_REPLY_ERROR))) {
        if (is_err) {
            if (err != NULL) {
                *err = zmalloc((r->len + 1) * sizeof(char));
                strcpy(*err, r->str);
            } else CLUSTER_PRINT_REPLY_ERROR(n, r->str);
        }
        return 0;
    }
    return 1;
}

redisCluster *createCluster(int numthreads) {
    redisCluster *cluster = zcalloc(sizeof(*cluster));
    if (!cluster) return NULL;
    cluster->numthreads = numthreads;
    cluster->nodes = listCreate();
    if (cluster->nodes == NULL) {
        zfree(cluster);
        return NULL;
    }
    cluster->slots_map = raxNew();
    if (cluster->slots_map == NULL) {
        listRelease(cluster->nodes);
        zfree(cluster);
        return NULL;
    }
    return cluster;
}

void freeClusterNode(clusterNode *node) {
    if (node == NULL) return;
    int i;
    if (node->context) {
        int numconnections = (node->cluster ? node->cluster->numthreads : 1);
        for (i = 0; i < numconnections; i++) {
            redisContext *ctx = node->context[i];
            if (ctx != NULL) redisFree(ctx);
        }
        zfree(node->context);
    }
    if (node->ip) sdsfree(node->ip);
    if (node->name) sdsfree(node->name);
    if (node->replicate) sdsfree(node->replicate);
    if (node->migrating != NULL) {
        for (i = 0; i < node->migrating_count; i++) sdsfree(node->migrating[i]);
        zfree(node->migrating);
    }
    if (node->importing != NULL) {
        for (i = 0; i < node->importing_count; i++) sdsfree(node->importing[i]);
        zfree(node->importing);
    }
    zfree(node->slots);
    zfree(node);
}

static void freeClusterNodes(redisCluster *cluster) {
    if (!cluster || !cluster->nodes) return;
    listIter li;
    listNode *ln;
    listRewind(cluster->nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterNode *node = ln->value;
        freeClusterNode(node);
    }
    listRelease(cluster->nodes);
}

void freeCluster(redisCluster *cluster) {
    raxFree(cluster->slots_map);
    freeClusterNodes(cluster);
    zfree(cluster);
}

clusterNode *createClusterNode(char *ip, int port, redisCluster *c) {
    clusterNode *node = zcalloc(sizeof(*node));
    if (!node) return NULL;
    node->cluster = c;
    /* If node will be part of the shared cluster, allocate n redisContext*
     * connections, one per thread. Otherwise, if the node will be part of
     * a client private redisClusterConnection, just create one redisContext*
     * connection. */
    int numconnections = (c ? c->numthreads : 1);
    node->context = zcalloc(numconnections * sizeof(redisContext *));
    node->ip = sdsnew(ip);
    node->port = port;
    node->name = NULL;
    node->flags = 0;
    node->replicate = NULL;
    node->replicas_count = 0;
    node->slots = zmalloc(CLUSTER_SLOTS * sizeof(int));
    node->slots_count = 0;
    node->migrating = NULL;
    node->importing = NULL;
    node->migrating_count = 0;
    node->importing_count = 0;
    node->clone_of = NULL;
    if (pthread_mutex_init(&(node->connection_mutex), NULL) != 0) {
        proxyLogErr("Failed to init connection_mutex on node %s:%d\n",
                     node->ip, node->port);
        freeClusterNode(node);
        return NULL;
    }
    return node;
}

clusterNode *duplicateClusterNode(clusterNode *source, redisCluster *c) {
    clusterNode *node = createClusterNode(source->ip, source->port, c);
    if (!node) return NULL;
    if (source->name) node->name = sdsdup(source->name);
    node->clone_of = source;
    return node;
}

redisContext *getClusterNodeConnection(clusterNode *node, int thread_id) {
    /* If the node is not part of a shared cluster, ie. when its part of
     * a client's private cluster connection, there's only one redisContext
     * so always take the first, regardless of thread_id. */
    if (node->cluster == NULL) return node->context[0];
    if (thread_id < 0) thread_id = node->cluster->numthreads - thread_id;
    if (thread_id >= node->cluster->numthreads) return NULL;
    return node->context[thread_id];
}

redisContext *clusterNodeConnect(clusterNode *node, int thread_id) {
    /* If the node is not part of a shared cluster, ie. when its part of
     * a client's private cluster connection, there's only one redisContext
     * so always take the first, regardless of thread_id. */
    if (node->cluster == NULL) thread_id = 0;
    redisContext *ctx = getClusterNodeConnection(node, thread_id);
    if (ctx) redisFree(ctx);
    proxyLogDebug("Connecting to node %s:%d\n", node->ip, node->port);
    ctx = redisConnect(node->ip, node->port);
    if (ctx->err) {
        proxyLogErr("Could not connect to Redis at ");
        proxyLogErr("%s:%d: %s\n", node->ip, node->port,
                    ctx->errstr);
        redisFree(ctx);
        node->context[thread_id] = NULL;
        return NULL;
    }
    /* Set aggressive KEEP_ALIVE socket option in the Redis context socket
     * in order to prevent timeouts caused by the execution of long
     * commands. At the same time this improves the detection of real
     * errors. */
    anetKeepAlive(NULL, ctx->fd, CLUSTER_NODE_KEEPALIVE_INTERVAL);
    if (config.auth) {
        redisReply *reply = redisCommand(ctx, "AUTH %s", config.auth);
        int ok = clusterCheckRedisReply(node, reply, NULL);
        if (reply != NULL) freeReplyObject(reply);
        if (!ok) {
            proxyLogErr("Failed to authenticate to %s:%d\n", node->ip,
                        node->port);
            redisFree(ctx);
            node->context[thread_id] = NULL;
            return NULL;
        }
    }
    node->context[thread_id] = ctx;
    return ctx;
}

redisContext *clusterNodeConnectAtomic(clusterNode *node, int thread_id) {
    pthread_mutex_lock(&(node->connection_mutex));
    redisContext *ctx = clusterNodeConnect(node, thread_id);
    pthread_mutex_unlock(&(node->connection_mutex));
    return ctx;
}

/* Map to slot into the cluster's radix tree map after converting the slot
 * to bigendian. */
void mapSlot(redisCluster *cluster, int slot, clusterNode *node) {
    uint32_t slot_be = htonl(slot);
    raxInsert(cluster->slots_map, (unsigned char *) &slot_be,
              sizeof(slot_be), node, NULL);
}

int fetchClusterConfiguration(redisCluster *cluster, char *ip, int port,
                              char *hostsocket)
{
    printf("Fetching cluster configuration...\n");
    int success = 1;
    redisContext *ctx = NULL;
    redisReply *reply =  NULL;
    if (hostsocket == NULL)
        ctx = redisConnect(ip, port);
    else
        ctx = redisConnectUnix(hostsocket);
    if (ctx->err) {
        fprintf(stderr, "Could not connect to Redis at ");
        if (hostsocket == NULL)
            fprintf(stderr,"%s:%d: %s\n", ip, port, ctx->errstr);
        else fprintf(stderr,"%s: %s\n", hostsocket, ctx->errstr);
        return 0;
    }
    clusterNode *firstNode = createClusterNode(ip, port, cluster);
    if (!firstNode) {success = 0; goto cleanup;}
    reply = redisCommand(ctx, "CLUSTER NODES");
    success = (reply != NULL);
    if (!success) goto cleanup;
    success = (reply->type != REDIS_REPLY_ERROR);
    if (!success) {
        fprintf(stderr, "Failed to retrieve cluster configuration.\n");
        if (hostsocket == NULL) {
            fprintf(stderr, "Cluster node %s:%d replied with error:\n%s\n",
                    ip, port, reply->str);
        } else {
            fprintf(stderr, "Cluster node %s replied with error:\n%s\n",
                    hostsocket, reply->str);
        }
        goto cleanup;
    }
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *name = NULL, *addr = NULL, *flags = NULL, *master_id = NULL;
        int i = 0;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            switch(i++){
            case 0: name = token; break;
            case 1: addr = token; break;
            case 2: flags = token; break;
            case 3: master_id = token; break;
            }
            if (i == 8) break; // Slots
        }
        if (!flags) {
            fprintf(stderr, "Invalid CLUSTER NODES reply: missing flags.\n");
            success = 0;
            goto cleanup;
        }
        if (addr == NULL) {
            fprintf(stderr, "Invalid CLUSTER NODES reply: missing addr.\n");
            success = 0;
            goto cleanup;
        }
        int myself = (strstr(flags, "myself") != NULL);
        clusterNode *node = NULL;
        char *ip = NULL;
        int port = 0;
        char *paddr = strchr(addr, ':');
        if (paddr != NULL) {
            *paddr = '\0';
            ip = addr;
            addr = paddr + 1;
            /* If internal bus is specified, then just drop it. */
            if ((paddr = strchr(addr, '@')) != NULL) *paddr = '\0';
            port = atoi(addr);
        }
        if (myself) {
            node = firstNode;
            if (node->ip == NULL && ip != NULL) {
                node->ip = ip;
                node->port = port;
            }
        } else {
            node = createClusterNode(ip, port, cluster);
        }
        if (node == NULL) {
            success = 0;
            goto cleanup;
        }
        if (name != NULL) node->name = sdsnew(name);
        node->is_replica = (strstr(flags, "slave") != NULL ||
                           (master_id != NULL && master_id[0] != '-'));
        if (i == 8) {
            int remaining = strlen(line);
            while (remaining > 0) {
                p = strchr(line, ' ');
                if (p == NULL) p = line + remaining;
                remaining -= (p - line);

                char *slotsdef = line;
                *p = '\0';
                if (remaining) {
                    line = p + 1;
                    remaining--;
                } else line = p;
                char *dash = NULL;
                if (slotsdef[0] == '[') {
                    slotsdef++;
                    if ((p = strstr(slotsdef, "->-"))) { // Migrating
                        *p = '\0';
                        p += 3;
                        char *closing_bracket = strchr(p, ']');
                        if (closing_bracket) *closing_bracket = '\0';
                        sds slot = sdsnew(slotsdef);
                        sds dst = sdsnew(p);
                        node->migrating_count += 2;
                        node->migrating =
                            zrealloc(node->migrating,
                                (node->migrating_count * sizeof(sds)));
                        node->migrating[node->migrating_count - 2] =
                            slot;
                        node->migrating[node->migrating_count - 1] =
                            dst;
                    }  else if ((p = strstr(slotsdef, "-<-"))) {//Importing
                        *p = '\0';
                        p += 3;
                        char *closing_bracket = strchr(p, ']');
                        if (closing_bracket) *closing_bracket = '\0';
                        sds slot = sdsnew(slotsdef);
                        sds src = sdsnew(p);
                        node->importing_count += 2;
                        node->importing = zrealloc(node->importing,
                            (node->importing_count * sizeof(sds)));
                        node->importing[node->importing_count - 2] =
                            slot;
                        node->importing[node->importing_count - 1] =
                            src;
                    }
                } else if ((dash = strchr(slotsdef, '-')) != NULL) {
                    p = dash;
                    int start, stop;
                    *p = '\0';
                    start = atoi(slotsdef);
                    stop = atoi(p + 1);
                    mapSlot(cluster, start, node);
                    mapSlot(cluster, stop, node);
                    while (start <= stop) {
                        int slot = start++;
                        node->slots[node->slots_count++] = slot;
                    }
                } else if (p > slotsdef) {
                    int slot = atoi(slotsdef);
                    node->slots[node->slots_count++] = slot;
                    mapSlot(cluster, slot, node);
                }
            }
        }
        listAddNodeTail(cluster->nodes, node);
    }
cleanup:
    freeReplyObject(reply);
    redisFree(ctx);
    if (!success) freeCluster(cluster);
    return success;
}

clusterNode *searchNodeByName(rax *nodes_map, sds name) {
    clusterNode *node = NULL;
    raxIterator iter;
    raxStart(&iter, nodes_map);
    if (!raxSeek(&iter, "=", (unsigned char*) name, sdslen(name))) {
        proxyLogErr("Failed to seek cluster node into nodes map.\n");
        raxStop(&iter);
        return NULL;
    }
    if (raxNext(&iter)) node = (clusterNode *) iter.data;
    raxStop(&iter);
    return node;
}

clusterNode *searchNodeBySlot(rax *slots_map, int slot) {
    clusterNode *node = NULL;
    raxIterator iter;
    raxStart(&iter, slots_map);
    int slot_be = htonl(slot);
    if (!raxSeek(&iter, ">=", (unsigned char*) &slot_be, sizeof(slot_be))) {
        proxyLogErr("Failed to seek cluster node into slots map.\n");
        raxStop(&iter);
        return NULL;
    }
    if (raxNext(&iter)) node = (clusterNode *) iter.data;
    raxStop(&iter);
    return node;
}

clusterNode *getNodeByKey(rax *slots_map, char *key, int keylen, int *getslot) {
    clusterNode *node = NULL;
    int slot = clusterKeyHashSlot(key, keylen);
    node = searchNodeBySlot(slots_map, slot);
    if (node && getslot != NULL) *getslot = slot;
    return node;
}

clusterNode *getFirstMappedNode(rax *map) {
    clusterNode *node = NULL;
    raxIterator iter;
    raxStart(&iter, map);
    if (!raxSeek(&iter, "^", NULL, 0)) {
        raxStop(&iter);
        return NULL;
    }
    if (raxNext(&iter)) node = (clusterNode *) iter.data;
    raxStop(&iter);
    return node;
}
