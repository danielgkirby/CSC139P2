#include "cachelab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------- FNV-1a hash (64-bit) ---------- */

static uint64_t fnv1a(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t bucket_index(cache_t *cache, const char *key) {
    return fnv1a(key) % cache->bucket_count;
}

/* ---------- Index find ---------- */

cache_entry_t *index_find(cache_t *cache, const char *key) {
    size_t bi = bucket_index(cache, key);
    cache_entry_t *cur = cache->hash_buckets[bi];
    while (cur) {
        if (cur->active && strcmp(cur->key, key) == 0)
            return cur;
        cur = cur->hash_next;
    }
    return NULL;
}

/* ---------- Index insert ---------- */

void index_insert(cache_t *cache, cache_entry_t *entry) {
    size_t bi = bucket_index(cache, entry->key);
    entry->hash_next = cache->hash_buckets[bi];
    cache->hash_buckets[bi] = entry;
    entry->active = 1;
}

/* ---------- Index remove ---------- */

void index_remove(cache_t *cache, cache_entry_t *entry) {
    if (!entry->active)
        return;

    size_t bi = bucket_index(cache, entry->key);
    cache_entry_t **pp = &cache->hash_buckets[bi];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->hash_next;
            entry->active = 0;
            entry->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/* ---------- Policy append ---------- */

void policy_append(cache_t *cache, cache_entry_t *entry) {
    entry->policy_next = NULL;
    entry->policy_prev = cache->policy_tail;

    if (cache->policy_tail)
        cache->policy_tail->policy_next = entry;
    else
        cache->policy_head = entry;

    cache->policy_tail = entry;

    if (!cache->clock_hand)
        cache->clock_hand = entry;
}

/* ---------- Policy remove ---------- */

void policy_remove(cache_t *cache, cache_entry_t *entry) {
    /* Caller removes this entry from the policy list, possibly after
     * index_remove has already marked it inactive in the hash index. */

    /* Repair clock hand if it points to this entry. */
    if (cache->clock_hand == entry) {
        cache_entry_t *succ = policy_successor(cache, entry);
        cache->clock_hand = (succ != entry) ? succ : NULL;
    }

    if (entry->policy_prev)
        entry->policy_prev->policy_next = entry->policy_next;
    else
        cache->policy_head = entry->policy_next;

    if (entry->policy_next)
        entry->policy_next->policy_prev = entry->policy_prev;
    else
        cache->policy_tail = entry->policy_prev;

    entry->policy_prev = NULL;
    entry->policy_next = NULL;
}

/* ---------- Policy successor ---------- */

cache_entry_t *policy_successor(cache_t *cache, cache_entry_t *entry) {
    if (entry->policy_next)
        return entry->policy_next;
    if (cache->policy_head && cache->policy_head != entry)
        return cache->policy_head;
    return entry;
}
