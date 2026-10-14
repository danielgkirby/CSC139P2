#include "cachelab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* ---------- Global dispatch state ---------- */

static request_t *dispatch_requests;
static size_t dispatch_count;
static size_t dispatch_next;
static int dispatch_stop;
static pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Worker routine (all threading and locking are provided) ---------- */

typedef struct {
    cache_t *cache;
    int client_id;
} worker_arg_t;

static void *worker_run(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    cache_t *cache = wa->cache;
    int cid = wa->client_id;

    for (;;) {
        pthread_mutex_lock(&dispatch_lock);
        if (dispatch_stop || dispatch_next >= dispatch_count) {
            pthread_mutex_unlock(&dispatch_lock);
            break;
        }
        request_t *req = &dispatch_requests[dispatch_next++];
        pthread_mutex_unlock(&dispatch_lock);

        pthread_mutex_lock(&cache->cache_lock);
        pthread_mutex_lock(&cache->output_lock);

        event_context_t ev;
        ev.req_seq = req->sequence;
        ev.client_id = cid;

        if (req->type == REQUEST_GET)
            handle_get(cache, req, &ev);
        else
            handle_put(cache, req, &ev);

        fflush(stdout);

        pthread_mutex_unlock(&cache->output_lock);
        pthread_mutex_unlock(&cache->cache_lock);
    }
    return NULL;
}

/* ---------- Policy name parsing ---------- */

static int parse_policy_arg(const char *arg) {
    if (strcmp(arg, "-a") == 0)
        return 1;
    if (strcmp(arg, "-e") == 0)
        return 2;
    return 0;
}

static int parse_policy_value(const char *val, int flag_kind,
                              alloc_policy_t *alloc_out,
                              eviction_policy_t *evict_out, int *parsed_alloc,
                              int *parsed_evict) {
    /* flag_kind 1 = -a (allocation), 2 = -e (eviction) */
    if (flag_kind == 1 && *parsed_alloc == 0 && strcmp(val, "first") == 0) {
        *alloc_out = ALLOC_FIRST_FIT;
        *parsed_alloc = 1;
        return 1;
    }
    if (flag_kind == 1 && *parsed_alloc == 0 && strcmp(val, "best") == 0) {
        *alloc_out = ALLOC_BEST_FIT;
        *parsed_alloc = 1;
        return 1;
    }
    if (flag_kind == 2 && *parsed_evict == 0 && strcmp(val, "fifo") == 0) {
        *evict_out = EVICT_FIFO;
        *parsed_evict = 1;
        return 1;
    }
    if (flag_kind == 2 && *parsed_evict == 0 && strcmp(val, "clock") == 0) {
        *evict_out = EVICT_CLOCK;
        *parsed_evict = 1;
        return 1;
    }
    return 0;
}

/* ---------- Main ---------- */

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s <pool_size> <trace_file> <num_clients>"
                " -a first|best -e fifo|clock\n", argv[0]);
        return 1;
    }

    /* Parse positional args */
    char *end = NULL;
    errno = 0;
    unsigned long long pool_val = strtoull(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || pool_val == 0 ||
        errno == ERANGE || pool_val > SIZE_MAX) {
        fprintf(stderr, "invalid pool_size\n");
        return 1;
    }
    size_t pool_size = (size_t)pool_val;

    char *trace_path = argv[2];

    long nc_long = strtol(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || nc_long < 1 || nc_long > 8) {
        fprintf(stderr, "invalid num_clients (must be 1-8)\n");
        return 1;
    }
    int num_clients = (int)nc_long;

    /* Parse policy options */
    alloc_policy_t alloc_pol = ALLOC_FIRST_FIT;
    eviction_policy_t evict_pol = EVICT_FIFO;
    int parsed_alloc = 0, parsed_evict = 0;

    for (int i = 4; i < argc; i++) {
        int kind = parse_policy_arg(argv[i]);
        if (kind == 0)
            continue;
        if (kind == 1) { /* -a */
            if (i + 1 >= argc) {
                fprintf(stderr, "-a requires a value\n");
                return 1;
            }
            if (!parse_policy_value(argv[++i], kind, &alloc_pol, &evict_pol,
                                    &parsed_alloc, &parsed_evict)) {
                fprintf(stderr, "unknown allocation policy: %s\n", argv[i]);
                return 1;
            }
        } else if (kind == 2) { /* -e */
            if (i + 1 >= argc) {
                fprintf(stderr, "-e requires a value\n");
                return 1;
            }
            if (!parse_policy_value(argv[++i], kind, &alloc_pol, &evict_pol,
                                    &parsed_alloc, &parsed_evict)) {
                fprintf(stderr, "unknown eviction policy: %s\n", argv[i]);
                return 1;
            }
        }
    }

    if (!parsed_alloc || !parsed_evict) {
        fprintf(stderr, "missing -a or -e policy\n");
        return 1;
    }

    /* Parse trace */
    request_t *requests = NULL;
    size_t req_count = 0;
    char *errmsg = NULL;
    if (parse_trace(trace_path, &requests, &req_count, &errmsg) != 0) {
        if (errmsg) {
            fprintf(stderr, "%s\n", errmsg);
            free(errmsg);
        } else {
            fprintf(stderr, "trace parse error\n");
        }
        return 1;
    }

    /* Initialize cache */
    cache_t cache;
    memset(&cache, 0, sizeof(cache));

    if (pool_init(&cache.pool, pool_size, alloc_pol) != 0) {
        fprintf(stderr, "pool init failed\n");
        free(requests);
        return 1;
    }

    cache.hash_buckets = calloc(BUCKET_COUNT, sizeof(cache_entry_t *));
    if (!cache.hash_buckets) {
        fprintf(stderr, "hash table allocation failed\n");
        pool_destroy(&cache.pool);
        free(requests);
        return 1;
    }
    cache.bucket_count = BUCKET_COUNT;
    cache.allocation_policy = alloc_pol;
    cache.eviction_policy = evict_pol;
    cache.policy_head = NULL;
    cache.policy_tail = NULL;
    cache.clock_hand = NULL;
    cache.hits = 0;
    cache.misses = 0;
    cache.evictions = 0;
    cache.rejects = 0;
    cache.occupancy = 0;

    pthread_mutex_init(&cache.cache_lock, NULL);
    pthread_mutex_init(&cache.output_lock, NULL);

    /* Set up dispatch */
    dispatch_requests = requests;
    dispatch_count = req_count;
    dispatch_next = 0;
    dispatch_stop = 0;

    /* Create threads */
    pthread_t *threads = malloc((size_t)num_clients * sizeof(pthread_t));
    if (!threads) {
        fprintf(stderr, "thread array allocation failed\n");
        pool_destroy(&cache.pool);
        free(cache.hash_buckets);
        free(requests);
        return 1;
    }

    int threads_created = 0;
    worker_arg_t *worker_args = malloc((size_t)num_clients * sizeof(worker_arg_t));
    if (!worker_args) {
        fprintf(stderr, "worker arg allocation failed\n");
        free(threads);
        pool_destroy(&cache.pool);
        free(cache.hash_buckets);
        free(requests);
        return 1;
    }
    for (int i = 0; i < num_clients; i++) {
        worker_args[i].cache = &cache;
        worker_args[i].client_id = i;

        int rc = pthread_create(&threads[i], NULL, worker_run, &worker_args[i]);
        if (rc != 0) {
            fprintf(stderr, "thread create failed: %s\n", strerror(rc));
            break;
        }
        threads_created++;
    }

    if (threads_created < num_clients) {
        /* Partial failure: stop dispatch, join created threads */
        pthread_mutex_lock(&dispatch_lock);
        dispatch_stop = 1;
        pthread_mutex_unlock(&dispatch_lock);
    }

    /* Join all created threads */
    for (int i = 0; i < threads_created; i++)
        pthread_join(threads[i], NULL);

    /* Print STATS only if all threads were created */
    if (threads_created == num_clients) {
        pthread_mutex_lock(&cache.output_lock);
        printf("STATS hits=%zu misses=%zu evictions=%zu rejects=%zu"
               " final_occupancy=%zu/%zu\n",
               cache.hits, cache.misses, cache.evictions,
               cache.rejects, cache.occupancy, cache.pool.size);
        fflush(stdout);
        pthread_mutex_unlock(&cache.output_lock);
    } else {
        /* Partial failure: no STATS, exit nonzero */
        free(worker_args);
        free(threads);
        pool_destroy(&cache.pool);
        free(cache.hash_buckets);
        free(requests);
        return 1;
    }

    /* Cleanup */
    free(worker_args);
    free(threads);
    pool_destroy(&cache.pool);
    free(cache.hash_buckets);
    free(requests);
    pthread_mutex_destroy(&cache.cache_lock);
    pthread_mutex_destroy(&cache.output_lock);
    return 0;
}
