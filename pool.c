#include "cachelab.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------- Provided pointer and alignment helpers ---------- */

static inline size_t round_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static inline block_header_t *header_from_payload(void *payload) {
    return (block_header_t *)((unsigned char *)payload - HEADER_SIZE);
}

static inline void *payload_from_header(block_header_t *h) {
    return (void *)((unsigned char *)h + HEADER_SIZE);
}

#define UNFINISHED(name) do { \
    fprintf(stderr, "TODO pool.c: implement %s\n", name); \
    exit(EXIT_FAILURE); \
} while (0)

/* ---------- Provided pool initialization ---------- */

int pool_init(memory_pool_t *pool, size_t size, alloc_policy_t policy) {
    if (size < MIN_BLOCK_SIZE)
        return -1;

    pool->bytes = (unsigned char *)malloc(size);
    if (!pool->bytes)
        return -1;

    block_header_t *first = (block_header_t *)pool->bytes;
    first->size = size - HEADER_SIZE;
    first->free = 1;
    first->next = NULL;
    pool->blocks = first;
    pool->size = size;
    pool->policy = policy;
    return 0;
}

/* ---------- Student work: search the embedded block list ---------- */

static block_header_t *first_fit(memory_pool_t *pool, size_t requested) {
    /* TODO: Walk from pool->blocks. Return the first FREE block with payload
     * capacity >= requested, or NULL. The list also contains allocated blocks. */
    for (block_header_t *current = pool -> blocks;
        current != NULL;
        current = current -> next){
            if (current -> free && current -> size >= requested)
                return current;
        }
        return NULL;
}

static block_header_t *best_fit(memory_pool_t *pool, size_t requested) {
    /* TODO: Inspect the entire address-ordered list; choose the smallest
     * sufficient FREE block. On equal capacities keep the earlier address.
     * Return NULL if none fits. */
    block_header_t *best = NULL;
    
     for (block_header_t *current = pool -> blocks;
        current != NULL;
        current = current -> next){
            //If currentsize == requestedsize
            if (current -> free && 
                current -> size == requested )
                return current;
            
            // if it fits and best is null, or if it fits and current is better than best
            if (current -> free && 
                current -> size >= requested &&
                (best == NULL || current -> size < best -> size ) )
                best = current;
        }
        return best;
 
}

/* ---------- Student work: carve a second header inside the pool ---------- */

static void split_if_possible(block_header_t *block, size_t requested) {
    /* TODO: The new header starts at payload_from_header(block) +
     * round_up(requested, HEADER_ALIGNMENT). Split only when the space left
     * BEFORE the new header is at least MIN_BLOCK_SIZE (a header plus a
     * payload byte). Check the rounded size before subtracting it.
     * Link the new FREE block into the address-ordered list and set both
     * payload capacities. If no valid split fits, leave block->size alone. */
    size_t physical = round_up(requested, HEADER_ALIGNMENT);

    //remaining phyisical space is too small for new block
    if (physical > block->size ||
        block->size - physical < MIN_BLOCK_SIZE)
        return;

    //new block header for remaining space after requested block
    block_header_t *remainder =
        (block_header_t *)((unsigned char *)payload_from_header(block)
                           + physical);

    remainder->size = block->size - physical - HEADER_SIZE;
    remainder->free = 1;
    remainder->next = block->next;

    block->size = physical;
    block->next = remainder;
}


/* ---------- Student work: select, split and occupy ---------- */

void *pool_try_alloc(memory_pool_t *pool, size_t requested) {
    /* TODO: Dispatch to first_fit or best_fit according to pool->policy.
     * Return NULL without changing the list when no block fits. Otherwise
     * split if possible, mark the selected block allocated and return its
     * payload. Do not evict here: the cache handler owns that retry loop.
     * The caller already holds the provided cache/output locks. */

    block_header_t *block;

    if (pool->policy == ALLOC_FIRST_FIT)
        block = first_fit(pool, requested);
    else
        block = best_fit(pool, requested);

    if (block == NULL)
        return NULL;

    split_if_possible(block, requested);
    block->free = 0;
    return payload_from_header(block);
}

/* ---------- Student work: free and coalesce ---------- */

void pool_free(memory_pool_t *pool, void *payload, event_context_t *ev) {
    /* TODO: NULL is a no-op. Recover the header, mark it free, absorb a
     * free successor if present, then walk from pool->blocks to find and
     * absorb into a free predecessor if present. A merge adds the removed
     * header's bytes as well as its payload to the surviving block; repair
     * next links. Emit one COALESCE per actual merge via output_event(ev,...)
     * with req/client IDs, surviving payload offset and new payload size.
     * No mutex operations belong here; the worker already holds the locks. */
    
    if (payload == NULL)
        return;

    block_header_t *block = header_from_payload(payload);
    block->free = 1;

    /* Merge with the next block if it is free. */
    if (block->next != NULL && block->next->free) {
        block_header_t *next = block->next;

        block->size += HEADER_SIZE + next->size;
        block->next = next->next;

        output_event(ev, "COALESCE req=%zu client=%d addr=%zu size=%zu",
                     ev->req_seq, ev->client_id,
                     pool_offset(pool, payload_from_header(block)),
                     block->size);
    }

    /* Find the block immediately before this one. */
    block_header_t *previous = NULL;
    for (block_header_t *current = pool->blocks;
         current != NULL && current != block;
         current = current->next) {
        previous = current;
    }

    /* Merge with that predecessor if it is free. */
    if (previous != NULL && previous->free) {
        previous->size += HEADER_SIZE + block->size;
        previous->next = block->next;

        output_event(ev, "COALESCE req=%zu client=%d addr=%zu size=%zu",
                     ev->req_seq, ev->client_id,
                     pool_offset(pool, payload_from_header(previous)),
                     previous->size);
    }
}
    

/* ---------- Provided inspection and teardown ---------- */

size_t pool_offset(memory_pool_t *pool, void *payload) {
    return (size_t)((unsigned char *)payload - pool->bytes);
}

size_t pool_payload_capacity(void *payload) {
    block_header_t *h = header_from_payload(payload);
    return h->size;
}

void pool_inspect(memory_pool_t *pool, size_t *free_payload,
                  size_t *free_blocks, size_t *largest_free) {
    *free_payload = 0;
    *free_blocks = 0;
    *largest_free = 0;
    block_header_t *cur = pool->blocks;
    while (cur) {
        if (cur->free) {
            (*free_blocks)++;
            *free_payload += cur->size;
            if (cur->size > *largest_free)
                *largest_free = cur->size;
        }
        cur = cur->next;
    }
}

void print_pool(memory_pool_t *pool) {
    fprintf(stderr, "Pool %p size=%zu\n", (void *)pool->bytes, pool->size);
    block_header_t *cur = pool->blocks;
    while (cur) {
        fprintf(stderr, "  block=%p size=%zu free=%d next=%p\n",
                (void *)cur, cur->size, cur->free, (void *)cur->next);
        cur = cur->next;
    }
}

void pool_destroy(memory_pool_t *pool) {
    free(pool->bytes);
    pool->bytes = NULL;
    pool->blocks = NULL;
}
