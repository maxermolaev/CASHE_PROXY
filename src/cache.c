#include "cache.h"

#include <errno.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "../include/log.h"

#define MIN(x, y) (x < y) ? x : y

typedef struct cache_node_t {
    cache_entry_t *entry;
    struct timeval last_modified_time;
    pthread_rwlock_t rwlock;
    struct cache_node_t *next;
} cache_node_t;

struct cache_t {
    int capacity;
    cache_node_t **array;

    atomic_int garbage_collector_running;
    time_t entry_expired_time_ms;
    pthread_t garbage_collector;
};

static int hash(const char *request, size_t request_len, int size);
static cache_node_t *cache_node_create(cache_entry_t *entry);
static void cache_node_destroy(cache_node_t *node);
static void *garbage_collector_routine(void *arg);

cache_t *cache_create(int capacity, time_t cache_expired_time_ms) {
    errno = 0;
    cache_t *cache = malloc(sizeof(cache_t));
    if (cache == NULL) {
        if (errno == ENOMEM) log("Cache creation error: %s", strerror(errno));
        else log("Cache creation error: failed to reallocate memory");
        return NULL;
    }

    cache->capacity = capacity;
    cache->entry_expired_time_ms = cache_expired_time_ms;
    cache->garbage_collector_running = 1;

    errno = 0;
    cache->array = calloc(capacity, sizeof(cache_node_t *));
    if (cache->array == NULL) {
        if (errno == ENOMEM) log("Cache creation error: %s", strerror(errno));
        else log("Cache creation error: failed to reallocate memory");

        free(cache);
        return NULL;
    }
    for (int i = 0; i < capacity; i++) cache->array[i] = NULL;

    pthread_create(&cache->garbage_collector, NULL, garbage_collector_routine, cache);

    return cache;
}

cache_entry_t *cache_get(cache_t *cache, const char *request, size_t request_len) {
    if (cache == NULL) {
        log("Cache getting error: cache is NULL");
        return NULL;
    }

    int index = hash(request, request_len, cache->capacity);
    cache_node_t *curr = cache->array[index];

    cache_node_t *prev = NULL;
    while (curr != NULL) {
        pthread_rwlock_rdlock(&curr->rwlock);

        if (curr->entry->request_len == request_len && strncmp(curr->entry->request, request, request_len) == 0) {
            gettimeofday(&curr->last_modified_time, 0);
            pthread_rwlock_unlock(&curr->rwlock);
            return curr->entry;
        }

        prev = curr;
        curr = curr->next;
        pthread_rwlock_unlock(&prev->rwlock);
    }
    return NULL;
}

int cache_add(cache_t *cache, cache_entry_t *entry) {
    if (cache == NULL) {
        log("Cache adding error: cache is NULL");
        return ERROR;
    }
    if (entry == NULL) {
        log("Cache adding error: cache entry is NULL");
        return ERROR;
    }

    cache_node_t *node = cache_node_create(entry);
    if (node == NULL) return ERROR;

    int index = hash(entry->request, entry->request_len, cache->capacity);

    pthread_rwlock_wrlock(&node->rwlock);
    node->next = cache->array[index];
    pthread_rwlock_unlock(&node->rwlock);

    cache->array[index] = node;

    log("Add new cache entry");
    return SUCCESS;
}

int cache_delete(cache_t *cache, const char *request, size_t request_len) {
    if (cache == NULL) {
        log("Cache deleting error: cache is NULL");
        return ERROR;
    }

    int index = hash(request, request_len, cache->capacity);
    cache_node_t *curr = cache->array[index];

    if (curr == NULL) return NOT_FOUND;

    cache_node_t *prev = NULL;
    while (curr != NULL) {
        pthread_rwlock_rdlock(&curr->rwlock);

        if (curr->entry->request_len == request_len && strncmp(curr->entry->request, request, request_len) == 0) {
            if (prev == NULL) {
                cache_node_t *next = curr->next;
                if (next == NULL) cache->array[index] = NULL;
            } else {
                pthread_rwlock_wrlock(&prev->rwlock);
                prev->next = curr->next;
                pthread_rwlock_unlock(&prev->rwlock);
            }

            pthread_rwlock_unlock(&curr->rwlock);
            cache_node_destroy(curr);
            log("Cache entry destroy");
            return SUCCESS;
        }

        prev = curr;
        curr = curr->next;

        pthread_rwlock_unlock(&prev->rwlock);
    }

    return NOT_FOUND;
}

void cache_destroy(cache_t *cache) {
    if (cache == NULL) {
        log("Cache destroying error: cache is NULL");
        return;
    }

    cache->garbage_collector_running = 0;

    const int max_wait_sec = 5;
    int waited = 0;

    while (waited < max_wait_sec) {
        if (pthread_kill(cache->garbage_collector, 0) != 0) {
            break;
        }
        sleep(1);
        waited++;
    }
    pthread_detach(cache->garbage_collector);

    for (int i = 0; i < cache->capacity; i++) {
        cache_node_t *curr = cache->array[i];
        while (curr != NULL) {
            cache_node_t *next = curr->next;
            log("Delete entry: %s", curr->entry->request);
            cache_node_destroy(curr);
            curr = next;
        }
    }

    free(cache->array);
    free(cache);
}

static int hash(const char *request, size_t request_len, int size) {
    if (request == NULL) return 0;

    int hash_value = 0;
    for (size_t i = 0; i < request_len; i++) hash_value = (hash_value * 31 + request[i]) % size;
    return hash_value;
}

static cache_node_t *cache_node_create(cache_entry_t *entry) {
    errno = 0;
    cache_node_t *node = malloc(sizeof(cache_node_t));
    if (node == NULL) {
        if (errno == ENOMEM) log("Cache node creation error: %s", strerror(errno));
        else log("Cache node creation error: failed to reallocate memory");
        return NULL;
    }

    node->entry = entry;
    gettimeofday(&node->last_modified_time, 0);
    pthread_rwlock_init(&node->rwlock, NULL);
    node->next = NULL;

    return node;
}

static void cache_node_destroy(cache_node_t *node) {
    if (node == NULL) {
        log("Cache node destroying error: node is NULL");
        return;
    }
    cache_entry_destroy(node->entry);
    pthread_rwlock_destroy(&node->rwlock);
    free(node);
}

static void *garbage_collector_routine(void *arg) {
    pthread_setname_np("garbage-collector");
    if (arg == NULL) {
        log("Cache garbage collector error: cache is NULL");
        pthread_exit(NULL);
    }
    cache_t *cache = (cache_t *) arg;
    log("Cache garbage collector start");

    struct timeval curr_time;
    while (cache->garbage_collector_running) {
        usleep(MIN(1000 * cache->entry_expired_time_ms / 2, 1000000));
        //log("Garbage collector running");

        gettimeofday(&curr_time, 0);
        for (int i = 0; i < cache->capacity; i++) {
            cache_node_t *curr = cache->array[i];

            if (curr == NULL) continue;

            pthread_rwlock_rdlock(&curr->rwlock);

            cache_node_t *next = NULL;
            while (curr != NULL) {
                time_t diff = (curr_time.tv_sec - curr->last_modified_time.tv_sec) * 1000 +
                        (curr_time.tv_usec - curr->last_modified_time.tv_usec) / 1000;
                if (diff >= cache->entry_expired_time_ms) {
                    pthread_rwlock_rdlock(&curr->rwlock);
                    next = curr->next;
                    char *request = curr->entry->request;
                    size_t request_len = curr->entry->request_len;
                    pthread_rwlock_unlock(&curr->rwlock);

                    cache_delete(cache, request, request_len);
                } else {
                    pthread_rwlock_rdlock(&curr->rwlock);
                    next = curr->next;
                    pthread_rwlock_unlock(&curr->rwlock);
                }

                curr = next;
            }
        }
    }

    log("Cache garbage collector destroy");
    pthread_exit(NULL);
}