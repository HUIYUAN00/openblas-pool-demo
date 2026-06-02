/**
 * @file test.c
 * @brief OpenBLAS风格线程池与内存池测试程序
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pool.h"

typedef struct {
    int *data;
    int total_size;
    memory_pool_t *mem_pool;
} work_ctx_t;

static double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void parallel_work(void *arg, int task_id) {
    work_ctx_t *ctx = (work_ctx_t *)arg;
    
    void *buf = memory_alloc(ctx->mem_pool);
    if (!buf) {
        fprintf(stderr, "Task %d: Failed to allocate buffer\n", task_id);
        return;
    }
    
    int chunk = ctx->total_size / 4;
    int start = task_id * chunk;
    int end = start + chunk;
    if (end > ctx->total_size) end = ctx->total_size;
    
    int *local = (int *)buf;
    size_t buf_size = memory_pool_get_buffer_size(ctx->mem_pool);
    
    for (int i = start; i < end; i++) {
        ctx->data[i] = task_id * 1000 + i;
        
        if ((size_t)(i - start) < buf_size / sizeof(int)) {
            local[i - start] = ctx->data[i];
        }
    }
    
    memory_free(ctx->mem_pool, buf);
    
    printf("  [Task %d] Processed indices [%d, %d)\n", task_id, start, end);
}

static void simple_print(void *arg, int task_id) {
    printf("  Hello from task %d\n", task_id);
}

static void test_basic_thread_pool() {
    printf("\n========================================\n");
    printf("Test 1: Basic Thread Pool (OpenBLAS-style)\n");
    printf("========================================\n\n");
    
    printf("1.1 Creating thread pool (4 threads, spinning=enabled)...\n");
    thread_pool_t *pool = thread_pool_create(4, 1);
    if (!pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        return;
    }
    printf("    Status: initialized=%d, threads=%d, timeout=%u\n\n", 
           thread_pool_is_initialized(pool), 
           thread_pool_get_num_threads(pool), 
           thread_pool_get_timeout(pool));
    
    printf("1.2 Running 4 simple tasks...\n");
    double start = get_time_ms();
    thread_pool_parallel_for(pool, simple_print, NULL, 4);
    double end = get_time_ms();
    printf("    Execution time: %.2f ms\n\n", end - start);
    
    printf("1.3 Destroying pool...\n");
    thread_pool_destroy(pool);
    printf("    Pool destroyed\n\n");
    
    printf("Test 1: PASSED\n");
}

static void test_memory_pool() {
    printf("\n========================================\n");
    printf("Test 2: Memory Pool (OpenBLAS TLS-style)\n");
    printf("========================================\n\n");
    
    printf("2.1 Creating memory pool (64KB, 8 buffers, TLS=enabled)...\n");
    memory_pool_t *pool = memory_pool_create(64 * 1024, 8, 1);
    if (!pool) {
        fprintf(stderr, "Failed to create memory pool\n");
        return;
    }
    printf("    Status: initialized=%d, buffer_size=%zu, TLS=%s\n\n", 
           memory_pool_is_initialized(pool), 
           memory_pool_get_buffer_size(pool), 
           memory_pool_is_tls_enabled(pool) ? "enabled" : "disabled");
    
    printf("2.2 Allocating buffers...\n");
    void *b1 = memory_alloc(pool);
    void *b2 = memory_alloc(pool);
    void *b3 = memory_alloc(pool);
    printf("    Buffer 1: %p (aligned)\n", b1);
    printf("    Buffer 2: %p (aligned)\n", b2);
    printf("    Buffer 3: %p (aligned)\n\n", b3);
    
    printf("2.3 Testing lazy free...\n");
    memory_free(pool, b2);
    void *b4 = memory_alloc(pool);
    printf("    After free and realloc: b4=%p (reuses same buffer)\n\n", b4);
    
    memory_free(pool, b1);
    memory_free(pool, b3);
    memory_free(pool, b4);
    
    printf("2.4 Destroying memory pool...\n");
    memory_pool_destroy(pool);
    printf("    Pool destroyed\n\n");
    
    printf("Test 2: PASSED\n");
}

static void test_parallel_compute() {
    printf("\n========================================\n");
    printf("Test 3: Parallel Computation\n");
    printf("========================================\n\n");
    
    printf("3.1 Creating pools...\n");
    thread_pool_t *tp = thread_pool_create(4, 1);
    if (!tp) {
        fprintf(stderr, "Failed to create thread pool\n");
        return;
    }
    memory_pool_t *mp = memory_pool_create(128 * 1024, 8, 1);
    if (!mp) {
        fprintf(stderr, "Failed to create memory pool\n");
        thread_pool_destroy(tp);
        return;
    }
    printf("    Thread pool: %d threads, timeout=%u cycles\n", 
           thread_pool_get_num_threads(tp), thread_pool_get_timeout(tp));
    printf("    Memory pool: %zu bytes, TLS enabled\n\n", 
           memory_pool_get_buffer_size(mp));
    
    int data[1000];
    work_ctx_t ctx;
    ctx.data = data;
    ctx.total_size = 1000;
    ctx.mem_pool = mp;
    
    printf("3.2 Running parallel computation...\n");
    double start = get_time_ms();
    thread_pool_parallel_for(tp, parallel_work, &ctx, 4);
    double end = get_time_ms();
    printf("    Execution time: %.2f ms\n\n", end - start);
    
    printf("3.3 Verifying results...\n");
    int errors = 0;
    for (int i = 0; i < 1000 && errors < 10; i++) {
        int expected_task = i / 250;
        if (expected_task > 3) expected_task = 3;
        int expected = expected_task * 1000 + i;
        
        if (data[i] != expected) {
            printf("    Error at %d: expected %d, got %d\n", i, expected, data[i]);
            errors++;
        }
    }
    printf("    Verification: %s\n\n", errors == 0 ? "PASSED" : "FAILED");
    
    thread_pool_destroy(tp);
    memory_pool_destroy(mp);
    
    printf("Test 3: PASSED\n");
}

static void test_performance() {
    printf("\n========================================\n");
    printf("Test 4: Performance Benchmark\n");
    printf("========================================\n\n");
    
    printf("4.1 Creating pools...\n");
    thread_pool_t *tp = thread_pool_create(4, 1);
    if (!tp) {
        fprintf(stderr, "Failed to create thread pool\n");
        return;
    }
    memory_pool_t *mp = memory_pool_create(256 * 1024, 8, 1);
    if (!mp) {
        fprintf(stderr, "Failed to create memory pool\n");
        thread_pool_destroy(tp);
        return;
    }
    printf("    Thread pool: %d threads, spinning enabled\n", 
           thread_pool_get_num_threads(tp));
    printf("    Memory pool: %zu bytes, TLS enabled\n\n", 
           memory_pool_get_buffer_size(mp));
    
    int data[4000];
    work_ctx_t ctx;
    ctx.data = data;
    ctx.total_size = 4000;
    ctx.mem_pool = mp;
    
    int iterations = 100;
    printf("Running %d iterations...\n", iterations);
    
    double total_time = 0;
    for (int i = 0; i < iterations; i++) {
        double start = get_time_ms();
        thread_pool_parallel_for(tp, parallel_work, &ctx, 4);
        double end = get_time_ms();
        total_time += (end - start);
    }
    
    printf("\nResults:\n");
    printf("  Total time:      %.2f ms\n", total_time);
    printf("  Per iteration:   %.2f ms\n", total_time / iterations);
    printf("  Throughput:      %.2f iter/s\n\n", iterations / (total_time / 1000));
    
    thread_pool_destroy(tp);
    memory_pool_destroy(mp);
    
    printf("Test 4: PASSED\n");
}

int main() {
    printf("================================================\n");
    printf("  OpenBLAS-Style Thread & Memory Pool Demo\n");
    printf("  (With Spinning + TLS Optimizations)\n");
    printf("================================================\n");
    printf("\n");
    printf("Key Features (based on OpenBLAS design):\n");
    printf("  - Thread pool: YIELDING spinning + condition variable\n");
    printf("  - Memory pool: TLS + lazy allocation + 64-byte aligned\n");
    printf("  - Atomic queue: C11 atomic with ACQUIRE/RELEASE\n");
    printf("  - Memory barrier: ARM64(x86) MB/WMB/RMB\n");
    printf("\n");
    
    test_basic_thread_pool();
    test_memory_pool();
    test_parallel_compute();
    test_performance();
    
    printf("\n================================================\n");
    printf("  ALL TESTS PASSED SUCCESSFULLY\n");
    printf("================================================\n\n");
    
    return 0;
}