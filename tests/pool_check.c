#include "cachelab.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned coalesces;

/* Stand-in for the supplied cache output while testing the pool by itself. */
void output_event(event_context_t *ev, const char *fmt, ...) {
    (void)ev;
    if (strncmp(fmt, "COALESCE", 8) == 0)
        coalesces++;
}

static void check(int condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "pool check failed: %s\n", description);
        exit(EXIT_FAILURE);
    }
}

/* Check the embedded list against the byte region it describes. */
static size_t validate(memory_pool_t *pool) {
    size_t offset = 0;
    size_t count = 0;
    block_header_t *block = pool->blocks;
    check(block != NULL, "pool has a first header");

    while (block) {
        check(count++ < pool->size / MIN_BLOCK_SIZE + 1,
              "block list terminates");
        check(offset <= pool->size && pool->size - offset >= HEADER_SIZE,
              "header fits in the pool");
        check((uintptr_t)block == (uintptr_t)pool->bytes + offset,
              "headers follow each other in address order");
        check((uintptr_t)block % HEADER_ALIGNMENT == 0,
              "header is aligned");
        check(block->free == 0 || block->free == 1,
              "free flag is valid");
        check(block->size <= pool->size - offset - HEADER_SIZE,
              "payload fits before the pool end");

        offset += HEADER_SIZE + block->size;
        if (block->next) {
            check(offset <= pool->size &&
                  pool->size - offset >= HEADER_SIZE,
                  "next header fits in the pool");
            check((uintptr_t)block->next == (uintptr_t)pool->bytes + offset,
                  "next points to the following embedded header");
            check(!(block->free && block->next->free),
                  "adjacent free blocks were coalesced");
        }
        block = block->next;
    }
    check(offset == pool->size, "headers and payloads cover the entire pool");
    return count;
}

static void test_splitting(void) {
    memory_pool_t pool;
    event_context_t ev = {0, 0};
    check(pool_init(&pool, 256, ALLOC_FIRST_FIT) == 0,
          "initialize a small pool");
    check(validate(&pool) == 1, "initial pool is one free block");

    void *first = pool_try_alloc(&pool, 100);
    check(first == pool.bytes + HEADER_SIZE, "first payload follows header");
    check(validate(&pool) == 2, "a large remainder becomes a second block");
    check(pool.blocks->free == 0 && pool.blocks->next->free == 1,
          "split has occupied front and free remainder");
    check(pool_payload_capacity(first) >= 100, "allocated capacity is sufficient");

    pool_free(&pool, first, &ev);
    check(validate(&pool) == 1, "freeing adjacent to remainder coalesces");
    check(pool.blocks->size == pool.size - HEADER_SIZE,
          "free recovers the removed header bytes");
    pool_free(&pool, NULL, &ev);
    check(validate(&pool) == 1, "freeing NULL is a no-op");
    pool_destroy(&pool);

    check(pool_init(&pool, 256, ALLOC_FIRST_FIT) == 0,
          "reinitialize for the small-remainder case");
    void *almost_full = pool_try_alloc(&pool, 220);
    check(almost_full != NULL, "near-maximum request succeeds");
    check(validate(&pool) == 1, "too-small remainder is absorbed");
    check(pool_payload_capacity(almost_full) == pool.size - HEADER_SIZE,
          "absorbed capacity remains in the allocated block");
    pool_destroy(&pool);
}

static void test_coalescing(void) {
    memory_pool_t pool;
    event_context_t ev = {0, 0};
    check(pool_init(&pool, 1024, ALLOC_FIRST_FIT) == 0,
          "initialize for two-sided coalescing");
    void *a = pool_try_alloc(&pool, 100);
    void *b = pool_try_alloc(&pool, 120);
    void *c = pool_try_alloc(&pool, 80);
    check(a && b && c, "allocate three adjacent objects");
    validate(&pool);

    pool_free(&pool, a, &ev);
    validate(&pool);
    pool_free(&pool, c, &ev);
    validate(&pool);
    coalesces = 0;
    pool_free(&pool, b, &ev);
    check(coalesces == 2, "middle block merges both forward and backward");
    check(validate(&pool) == 1, "two-sided coalescing restores one block");
    check(pool.blocks->size == pool.size - HEADER_SIZE,
          "whole empty-pool payload is recovered");
    pool_destroy(&pool);
}

static void test_placement(alloc_policy_t policy) {
    memory_pool_t pool;
    event_context_t ev = {0, 0};
    check(pool_init(&pool, 1024, policy) == 0,
          "initialize placement test");

    void *a = pool_try_alloc(&pool, 240);
    void *b = pool_try_alloc(&pool, 80);
    void *c = pool_try_alloc(&pool, 160);
    void *d = pool_try_alloc(&pool, 80);
    void *e = pool_try_alloc(&pool, 200);
    check(a && b && c && d && e, "establish occupied separators");
    pool_free(&pool, a, &ev);
    pool_free(&pool, c, &ev);
    validate(&pool);

    size_t free_payload, free_blocks, largest;
    pool_inspect(&pool, &free_payload, &free_blocks, &largest);
    check(free_blocks == 3 && free_payload > 400 && largest < 400,
          "free bytes are sufficient in total but fragmented");
    check(pool_try_alloc(&pool, 400) == NULL,
          "one contiguous block is required");
    validate(&pool);

    block_header_t *last = pool.blocks;
    while (last->next)
        last = last->next;
    check(last->free, "tail region is free");
    void *tail_payload = (unsigned char *)last + HEADER_SIZE;
    void *selected = pool_try_alloc(&pool, 100);
    check(selected == (policy == ALLOC_FIRST_FIT ? a : tail_payload),
          "first-fit uses early large hole; best-fit uses smallest tail hole");
    validate(&pool);
    pool_destroy(&pool);
}

int main(void) {
    test_splitting();
    test_coalescing();
    test_placement(ALLOC_FIRST_FIT);
    test_placement(ALLOC_BEST_FIT);
    puts("pool checks passed");
    return 0;
}
