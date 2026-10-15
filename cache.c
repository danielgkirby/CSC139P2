#include "cachelab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---------- Provided event output ---------- */

void output_event(event_context_t *ev, const char *fmt, ...) {
    (void)ev;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

#define UNFINISHED(name) do { \
    fprintf(stderr, "TODO cache.c: implement %s\n", name); \
    exit(EXIT_FAILURE); \
} while (0)

/* ---------- Student work: select a victim, but do not remove it ---------- */

cache_entry_t *evict_fifo_select(cache_t *cache) {
    /* TODO: Return the oldest active entry, or NULL when empty. GET does
     * not change the insertion order. */
    return cache->policy_head;
}

cache_entry_t *evict_clock_select(cache_t *cache) {
    /* TODO: Starting at the persistent clock_hand, inspect candidates in
     * policy_successor order (wraps at tail). If reference_bit is set,
     * clear it, advance the hand and continue. On a clear bit, advance the
     * hand to the successor and RETURN the candidate without unlinking it.
     * Return NULL for an empty cache. Provided policy_append initializes
     * the hand; policy_remove repairs it if removing the pointed-to entry.
     * Do not call policy_remove while selecting: evict_one does it later. */
    (void)cache;
    UNFINISHED("evict_clock_select");
}

/* ---------- Provided transaction: evict one selected active entry ---------- */

static void evict_one(cache_t *cache, cache_entry_t *victim, event_context_t *ev) {
    output_event(ev, "EVICT req=%zu client=%d key=%s addr=%zu size=%zu",
                 ev->req_seq, ev->client_id, victim->key,
                 pool_offset(&cache->pool, victim->payload),
                 victim->requested_size);

    index_remove(cache, victim);
    policy_remove(cache, victim);
    cache->occupancy -= victim->requested_size;
    cache->evictions++;
    pool_free(&cache->pool, victim->payload, ev);
}

/* ---------- Student work: GET request ---------- */

void handle_get(cache_t *cache, request_t *req, event_context_t *ev) {
    /* TODO: index_find(key); a hit sets reference_bit, increments hits and
     * emits HIT; a miss increments misses and emits MISS. GET does not
     * allocate, reorder the policy list or change the clock hand.
     * The provided worker holds cache_lock and output_lock already. */
    (void)cache;
    (void)req;
    (void)ev;
    UNFINISHED("handle_get");
}

/* ---------- Student work: PUT, update and eviction retry ---------- */

void handle_put(cache_t *cache, request_t *req, event_context_t *ev) {
    /* TODO: Emit PUT. Reject requested sizes above pool.size - HEADER_SIZE
     * before touching an existing key; emit REJECT and increment rejects.
     * For a valid existing key emit UPDATE, unlink it from index/policy,
     * subtract its requested occupancy and free its pool block. This is
     * NOT a policy eviction. Then call pool_try_alloc; while it returns
     * NULL select a FIFO/clock victim, call provided evict_one and retry.
     * Every valid PUT must eventually fit after enough evictions.
     * Initialize req->entry_storage (the parser-owned stable slot) with
     * the key, requested size, payload and reference_bit=1; insert into
     * index, append to the policy list, add requested occupancy, emit ALLOC.
     * No malloc of entries and no mutex operations: the worker holds both
     * cache_lock and output_lock throughout this request. */
    (void)cache;
    (void)req;
    (void)ev;
    (void)evict_one;
    UNFINISHED("handle_put");
}
