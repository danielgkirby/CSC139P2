#ifndef CACHELAB_H
#define CACHELAB_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* ---------- Constants ---------- */

#define HEADER_SIZE       (sizeof(struct block_header))
#define MIN_BLOCK_SIZE    (HEADER_SIZE + 1)
#define HEADER_ALIGNMENT  (_Alignof(struct block_header))
#define KEY_MAX           64
#define KEY_BUF           65
#define BUCKET_COUNT      257

/* ---------- Enums ---------- */

typedef enum {
    ALLOC_FIRST_FIT,
    ALLOC_BEST_FIT
} alloc_policy_t;

typedef enum {
    EVICT_FIFO,
    EVICT_CLOCK
} eviction_policy_t;

typedef enum {
    REQUEST_GET,
    REQUEST_PUT
} request_type_t;

/* ---------- Block header ---------- */

typedef struct block_header {
    size_t size;                    /* payload capacity, excludes header */
    int free;                       /* 1 when free, 0 when allocated */
    struct block_header *next;    /* next block in pool address order */
} block_header_t;

/* ---------- Memory pool ---------- */

typedef struct {
    unsigned char *bytes;
    size_t size;                    /* total pool size (incl. headers) */
    block_header_t *blocks;         /* first block header */
    alloc_policy_t policy;
} memory_pool_t;

/* ---------- Cache entry ---------- */

typedef struct cache_entry {
    char key[KEY_BUF];
    size_t requested_size;
    void *payload;
    int reference_bit;

    struct cache_entry *hash_next;
    struct cache_entry *policy_prev;
    struct cache_entry *policy_next;
    int active;
} cache_entry_t;

/* ---------- Request ---------- */

typedef struct request {
    size_t sequence;
    request_type_t type;
    char key[KEY_BUF];
    size_t size;                    /* PUT only */
    cache_entry_t entry_storage;    /* stable metadata slot for this PUT */
} request_t;

/* ---------- Cache state ---------- */

typedef struct {
    memory_pool_t pool;
    cache_entry_t **hash_buckets;
    size_t bucket_count;
    cache_entry_t *policy_head;
    cache_entry_t *policy_tail;
    cache_entry_t *clock_hand;
    alloc_policy_t allocation_policy;
    eviction_policy_t eviction_policy;
    pthread_mutex_t cache_lock;
    pthread_mutex_t output_lock;

    size_t hits;
    size_t misses;
    size_t evictions;
    size_t rejects;
    size_t occupancy;
} cache_t;

/* ---------- Event context (carries request IDs for output) ---------- */

typedef struct {
    size_t req_seq;
    int client_id;
} event_context_t;

/* ---------- Pool API ---------- */

int      pool_init(memory_pool_t *pool, size_t size, alloc_policy_t policy);
void    *pool_try_alloc(memory_pool_t *pool, size_t requested);
void     pool_free(memory_pool_t *pool, void *payload, event_context_t *ev);
size_t   pool_offset(memory_pool_t *pool, void *payload);
size_t   pool_payload_capacity(void *payload);
void     pool_inspect(memory_pool_t *pool, size_t *free_payload,
                      size_t *free_blocks, size_t *largest_free);
void     print_pool(memory_pool_t *pool);
void     pool_destroy(memory_pool_t *pool);

/* ---------- Index API ---------- */

cache_entry_t *index_find(cache_t *cache, const char *key);
void           index_insert(cache_t *cache, cache_entry_t *entry);
void           index_remove(cache_t *cache, cache_entry_t *entry);

/* ---------- Policy list API ---------- */

/* These provided helpers initialize clock_hand on the first append and
 * advance it if the entry currently under the hand is removed. */
void policy_append(cache_t *cache, cache_entry_t *entry);
void policy_remove(cache_t *cache, cache_entry_t *entry);
cache_entry_t *policy_successor(cache_t *cache, cache_entry_t *entry);

/* ---------- Eviction ---------- */

cache_entry_t *evict_fifo_select(cache_t *cache);
cache_entry_t *evict_clock_select(cache_t *cache);

/* ---------- Request handlers ---------- */

/* The provided worker holds cache_lock, then output_lock, for the entire
 * handler call. These functions and their pool/index helpers do not lock. */
void handle_get(cache_t *cache, request_t *req, event_context_t *ev);
void handle_put(cache_t *cache, request_t *req, event_context_t *ev);

/* ---------- Trace parsing ---------- */

int parse_trace(const char *path, request_t **requests_out,
                size_t *count_out, char **errmsg);

/* ---------- Output ---------- */

void output_event(event_context_t *ev, const char *fmt, ...);

#endif /* CACHELAB_H */
