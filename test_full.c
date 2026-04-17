/**
 * @file test_full.c
 * @brief 线程池和内存池全面测试程序
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <assert.h>
#include "pool.h"

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define TEST_START(name) do { \
    tests_total++; \
    printf("\n[TEST %d] %s\n", tests_total, name); \
} while(0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf("  RESULT: PASSED\n"); \
} while(0)

#define TEST_FAIL(msg) do { \
    tests_failed++; \
    printf("  RESULT: FAILED - %s\n", msg); \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

static double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static atomic_int concurrent_counter;
static atomic_int task_executed[64];

static void simple_task(void *arg, int task_id) {
    (void)arg;
    task_executed[task_id] = 1;
}

static void counter_task(void *arg, int task_id) {
    (void)arg;
    (void)task_id;
    for (int i = 0; i < 1000; i++) {
        atomic_fetch_add(&concurrent_counter, 1);
    }
}

static void arg_pass_task(void *arg, int task_id) {
    int *data = (int *)arg;
    data[task_id] = task_id * 100 + task_id;
}

typedef struct {
    memory_pool_t *mp;
    int task_id;
    void *allocated;
} mem_task_arg_t;

static void mem_alloc_task(void *arg, int task_id) {
    mem_task_arg_t *ctx = (mem_task_arg_t *)arg;
    ctx[task_id].allocated = memory_alloc(ctx[task_id].mp);
}

static void sleep_task(void *arg, int task_id) {
    (void)arg;
    (void)task_id;
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}

static void use_mem_pool_task(void *arg, int task_id) {
    memory_pool_t *mp = (memory_pool_t *)arg;
    void *buf = memory_alloc(mp);
    if (buf) {
        memset(buf, task_id, 64);
        memory_free(mp, buf);
    }
}

static void null_task(void *arg, int task_id) {
    (void)arg;
    (void)task_id;
}

void test_tp_f01_normal_init() {
    TEST_START("TP-F01: 正常初始化线程池");
    
    thread_pool_t pool;
    int ret = thread_pool_init(&pool, 4);
    
    ASSERT_TRUE(ret == 0, "返回值应为0");
    ASSERT_TRUE(pool.initialized == 1, "initialized应为1");
    ASSERT_TRUE(pool.num_threads == 4, "线程数应为4");
    ASSERT_TRUE(pool.shutdown == 0, "shutdown应为0");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_f02_normal_shutdown() {
    TEST_START("TP-F02: 线程池正常关闭");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    thread_pool_shutdown(&pool);
    
    ASSERT_TRUE(pool.initialized == 0, "initialized应为0");
    TEST_PASS();
}

void test_tp_f03_parallel_execution() {
    TEST_START("TP-F03: 并行任务正确执行");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    memset(task_executed, 0, sizeof(task_executed));
    int ret = thread_pool_parallel_for(&pool, simple_task, NULL, 4);
    
    ASSERT_TRUE(ret == 0, "返回值应为0");
    
    int all_executed = 1;
    for (int i = 0; i < 4; i++) {
        if (!task_executed[i]) all_executed = 0;
    }
    ASSERT_TRUE(all_executed, "所有任务都应被执行");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_f04_task_completion_sync() {
    TEST_START("TP-F04: 任务完成同步等待");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    double start = get_time_ms();
    thread_pool_parallel_for(&pool, sleep_task, NULL, 4);
    double end = get_time_ms();
    
    ASSERT_TRUE((end - start) < 2000, "应并行执行而非串行");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_f05_arg_passing() {
    TEST_START("TP-F05: 任务函数参数传递");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    int data[4] = {0};
    thread_pool_parallel_for(&pool, arg_pass_task, data, 4);
    
    int correct = 1;
    for (int i = 0; i < 4; i++) {
        if (data[i] != i * 100 + i) correct = 0;
    }
    ASSERT_TRUE(correct, "参数应正确传递");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b01_zero_threads() {
    TEST_START("TP-B01: 线程数为0");
    
    thread_pool_t pool;
    int ret = thread_pool_init(&pool, 0);
    
    ASSERT_TRUE(ret == 0, "返回值应为0");
    ASSERT_TRUE(pool.num_threads == 4, "应使用默认值4");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b02_negative_threads() {
    TEST_START("TP-B02: 线程数为负数");
    
    thread_pool_t pool;
    int ret = thread_pool_init(&pool, -1);
    
    ASSERT_TRUE(ret == 0, "返回值应为0");
    ASSERT_TRUE(pool.num_threads == 4, "应使用默认值4");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b03_exceed_max_threads() {
    TEST_START("TP-B03: 线程数超过MAX_THREADS(64)");
    
    thread_pool_t pool;
    int ret = thread_pool_init(&pool, 100);
    
    ASSERT_TRUE(ret == 0, "返回值应为0");
    ASSERT_TRUE(pool.num_threads == 4, "应使用默认值4");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b04_zero_tasks() {
    TEST_START("TP-B04: 任务数为0");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    int ret = thread_pool_parallel_for(&pool, simple_task, NULL, 0);
    ASSERT_TRUE(ret == -1, "应返回-1");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b05_negative_tasks() {
    TEST_START("TP-B05: 任务数为负数");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    int ret = thread_pool_parallel_for(&pool, simple_task, NULL, -1);
    ASSERT_TRUE(ret == -1, "应返回-1");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b06_single_thread() {
    TEST_START("TP-B06: 单线程运行");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 1);
    
    memset(task_executed, 0, sizeof(task_executed));
    thread_pool_parallel_for(&pool, simple_task, NULL, 4);
    
    int all_executed = 1;
    for (int i = 0; i < 4; i++) {
        if (!task_executed[i]) all_executed = 0;
    }
    ASSERT_TRUE(all_executed, "单线程也应完成所有任务");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_b07_more_tasks_than_threads() {
    TEST_START("TP-B07: 任务数多于线程数");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 2);
    
    memset(task_executed, 0, sizeof(task_executed));
    thread_pool_parallel_for(&pool, simple_task, NULL, 8);
    
    int all_executed = 1;
    for (int i = 0; i < 8; i++) {
        if (!task_executed[i]) all_executed = 0;
    }
    ASSERT_TRUE(all_executed, "所有任务都应被执行");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_e01_uninit_parallel_for() {
    TEST_START("TP-E01: 未初始化调用parallel_for");
    
    thread_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    
    int ret = thread_pool_parallel_for(&pool, simple_task, NULL, 4);
    ASSERT_TRUE(ret == -1, "应返回-1");
    
    TEST_PASS();
}

void test_tp_e02_double_shutdown() {
    TEST_START("TP-E02: 重复关闭线程池");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    thread_pool_shutdown(&pool);
    thread_pool_shutdown(&pool);
    
    ASSERT_TRUE(pool.initialized == 0, "initialized应为0");
    TEST_PASS();
}

void test_tp_e03_use_after_shutdown() {
    TEST_START("TP-E03: 关闭后重新使用");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    thread_pool_shutdown(&pool);
    
    int ret = thread_pool_parallel_for(&pool, simple_task, NULL, 4);
    ASSERT_TRUE(ret == -1, "应返回-1");
    
    TEST_PASS();
}

void test_tp_e04_null_func() {
    TEST_START("TP-E04: 任务函数为NULL");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    int ret = thread_pool_parallel_for(&pool, NULL, NULL, 4);
    ASSERT_TRUE(ret == 0, "应正常返回(不崩溃)");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_c01_concurrent_correctness() {
    TEST_START("TP-C01: 多任务并发正确性");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    atomic_store(&concurrent_counter, 0);
    thread_pool_parallel_for(&pool, counter_task, NULL, 4);
    
    int expected = 4 * 1000;
    int actual = atomic_load(&concurrent_counter);
    ASSERT_TRUE(actual == expected, "原子计数应正确");
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_c04_mutex_protection() {
    TEST_START("TP-C04: 互斥锁保护正确");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 8);
    
    for (int i = 0; i < 10; i++) {
        memset(task_executed, 0, sizeof(task_executed));
        thread_pool_parallel_for(&pool, simple_task, NULL, 8);
    }
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_s01_many_iterations() {
    TEST_START("TP-S01: 大量任务循环(100次)");
    
    thread_pool_t pool;
    thread_pool_init(&pool, 4);
    
    for (int i = 0; i < 100; i++) {
        thread_pool_parallel_for(&pool, null_task, NULL, 4);
    }
    
    thread_pool_shutdown(&pool);
    TEST_PASS();
}

void test_tp_s02_max_threads() {
    TEST_START("TP-S02: 最大线程数(64)");
    
    thread_pool_t pool;
    int ret = thread_pool_init(&pool, 64);
    
    if (ret == 0) {
        thread_pool_parallel_for(&pool, simple_task, NULL, 4);
        thread_pool_shutdown(&pool);
        TEST_PASS();
    } else {
        TEST_FAIL("无法创建64线程");
    }
}

void test_mp_f01_normal_init() {
    TEST_START("MP-F01: 正常初始化内存池");
    
    memory_pool_t pool;
    int ret = memory_pool_init(&pool, 64 * 1024, 8);
    
    ASSERT_TRUE(ret == 0, "返回值应为0");
    ASSERT_TRUE(pool.initialized == 1, "initialized应为1");
    ASSERT_TRUE(pool.buffer_size == 64 * 1024, "buffer_size应正确");
    ASSERT_TRUE(pool.num_buffers == 8, "num_buffers应正确");
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_f02_normal_destroy() {
    TEST_START("MP-F02: 正常销毁内存池");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 64 * 1024, 8);
    memory_pool_destroy(&pool);
    
    ASSERT_TRUE(pool.initialized == 0, "initialized应为0");
    TEST_PASS();
}

void test_mp_f03_normal_alloc() {
    TEST_START("MP-F03: 正常分配buffer");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 64 * 1024, 8);
    
    void *buf = memory_alloc(&pool);
    ASSERT_TRUE(buf != NULL, "应返回非NULL指针");
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_f04_normal_free() {
    TEST_START("MP-F04: 正常释放buffer");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 64 * 1024, 8);
    
    void *buf = memory_alloc(&pool);
    memory_free(&pool, buf);
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_f05_buffer_reuse() {
    TEST_START("MP-F05: buffer复用");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 64 * 1024, 8);
    
    void *buf1 = memory_alloc(&pool);
    memory_free(&pool, buf1);
    void *buf2 = memory_alloc(&pool);
    
    ASSERT_TRUE(buf1 == buf2, "应复用相同地址");
    
    memory_free(&pool, buf2);
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_b01_zero_buffer_size() {
    TEST_START("MP-B01: buffer_size为0");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 0, 8);
    
    ASSERT_TRUE(pool.buffer_size == 64 * 1024, "应使用默认64KB");
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_b02_zero_num_buffers() {
    TEST_START("MP-B02: num_buffers为0");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 0);
    
    ASSERT_TRUE(pool.num_buffers == 8, "应使用默认值8");
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_b03_negative_num_buffers() {
    TEST_START("MP-B03: num_buffers为负数");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, -1);
    
    ASSERT_TRUE(pool.num_buffers == 8, "应使用默认值8");
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_b04_exceed_max_buffers() {
    TEST_START("MP-B04: num_buffers超过MAX(16)");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 100);
    
    ASSERT_TRUE(pool.num_buffers == 8, "应使用默认值8");
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_b05_alloc_all_buffers() {
    TEST_START("MP-B05: 分配全部buffer");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 8);
    
    void *bufs[8];
    int all_success = 1;
    for (int i = 0; i < 8; i++) {
        bufs[i] = memory_alloc(&pool);
        if (!bufs[i]) all_success = 0;
    }
    
    ASSERT_TRUE(all_success, "所有buffer都应分配成功");
    
    for (int i = 0; i < 8; i++) {
        memory_free(&pool, bufs[i]);
    }
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_b06_buffer_exhausted() {
    TEST_START("MP-B06: buffer耗尽");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 4);
    
    void *bufs[5];
    for (int i = 0; i < 4; i++) {
        bufs[i] = memory_alloc(&pool);
    }
    
    bufs[4] = memory_alloc(&pool);
    ASSERT_TRUE(bufs[4] == NULL, "耗尽后应返回NULL");
    
    for (int i = 0; i < 4; i++) {
        memory_free(&pool, bufs[i]);
    }
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_e01_uninit_alloc() {
    TEST_START("MP-E01: 未初始化分配");
    
    memory_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    
    void *buf = memory_alloc(&pool);
    ASSERT_TRUE(buf == NULL, "应返回NULL");
    
    TEST_PASS();
}

void test_mp_e02_uninit_free() {
    TEST_START("MP-E02: 未初始化释放");
    
    memory_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    
    char dummy[64];
    memory_free(&pool, dummy);
    
    TEST_PASS();
}

void test_mp_e03_free_null() {
    TEST_START("MP-E03: 释放NULL指针");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 4);
    
    memory_free(&pool, NULL);
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_e04_double_destroy() {
    TEST_START("MP-E04: 重复销毁");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 4);
    memory_pool_destroy(&pool);
    memory_pool_destroy(&pool);
    
    ASSERT_TRUE(pool.initialized == 0, "initialized应为0");
    TEST_PASS();
}

void test_mp_e05_free_invalid_ptr() {
    TEST_START("MP-E05: 释放非pool内指针");
    
    memory_pool_t pool;
    memory_pool_init(&pool, 1024, 4);
    
    char dummy[64];
    memory_free(&pool, dummy);
    
    memory_pool_destroy(&pool);
    TEST_PASS();
}

void test_mp_c01_concurrent_alloc() {
    TEST_START("MP-C01: 多线程并发分配");
    
    thread_pool_t tp;
    memory_pool_t mp;
    thread_pool_init(&tp, 4);
    memory_pool_init(&mp, 1024, 16);
    
    mem_task_arg_t args[8];
    for (int i = 0; i < 8; i++) {
        args[i].mp = &mp;
        args[i].task_id = i;
        args[i].allocated = NULL;
    }
    
    thread_pool_parallel_for(&tp, mem_alloc_task, args, 8);
    
    int all_allocated = 1;
    for (int i = 0; i < 8; i++) {
        if (!args[i].allocated) all_allocated = 0;
    }
    ASSERT_TRUE(all_allocated, "所有任务都应分配到buffer");
    
    for (int i = 0; i < 8; i++) {
        if (args[i].allocated) memory_free(&mp, args[i].allocated);
    }
    
    thread_pool_shutdown(&tp);
    memory_pool_destroy(&mp);
    TEST_PASS();
}

void test_mp_c03_alloc_free_alternate() {
    TEST_START("MP-C03: 分配释放交替");
    
    thread_pool_t tp;
    memory_pool_t mp;
    thread_pool_init(&tp, 4);
    memory_pool_init(&mp, 1024, 8);
    
    for (int i = 0; i < 10; i++) {
        thread_pool_parallel_for(&tp, use_mem_pool_task, &mp, 4);
    }
    
    thread_pool_shutdown(&tp);
    memory_pool_destroy(&mp);
    TEST_PASS();
}

void test_int_01_thread_memory_pool() {
    TEST_START("INT-01: 线程池+内存池协同");
    
    thread_pool_t tp;
    memory_pool_t mp;
    thread_pool_init(&tp, 4);
    memory_pool_init(&mp, 1024, 8);
    
    for (int i = 0; i < 5; i++) {
        thread_pool_parallel_for(&tp, use_mem_pool_task, &mp, 4);
    }
    
    thread_pool_shutdown(&tp);
    memory_pool_destroy(&mp);
    TEST_PASS();
}

void test_int_02_multi_init_destroy() {
    TEST_START("INT-02: 多轮初始化销毁");
    
    for (int i = 0; i < 10; i++) {
        thread_pool_t tp;
        memory_pool_t mp;
        thread_pool_init(&tp, 4);
        memory_pool_init(&mp, 1024, 4);
        
        thread_pool_parallel_for(&tp, use_mem_pool_task, &mp, 4);
        
        thread_pool_shutdown(&tp);
        memory_pool_destroy(&mp);
    }
    
    TEST_PASS();
}

void run_all_tests() {
    printf("\n");
    printf("================================================\n");
    printf("  线程池功能测试\n");
    printf("================================================\n");
    test_tp_f01_normal_init();
    test_tp_f02_normal_shutdown();
    test_tp_f03_parallel_execution();
    test_tp_f04_task_completion_sync();
    test_tp_f05_arg_passing();
    
    printf("\n");
    printf("================================================\n");
    printf("  线程池边界测试\n");
    printf("================================================\n");
    test_tp_b01_zero_threads();
    test_tp_b02_negative_threads();
    test_tp_b03_exceed_max_threads();
    test_tp_b04_zero_tasks();
    test_tp_b05_negative_tasks();
    test_tp_b06_single_thread();
    test_tp_b07_more_tasks_than_threads();
    
    printf("\n");
    printf("================================================\n");
    printf("  线程池异常测试\n");
    printf("================================================\n");
    test_tp_e01_uninit_parallel_for();
    test_tp_e02_double_shutdown();
    test_tp_e03_use_after_shutdown();
    test_tp_e04_null_func();
    
    printf("\n");
    printf("================================================\n");
    printf("  线程池并发测试\n");
    printf("================================================\n");
    test_tp_c01_concurrent_correctness();
    test_tp_c04_mutex_protection();
    
    printf("\n");
    printf("================================================\n");
    printf("  线程池压力测试\n");
    printf("================================================\n");
    test_tp_s01_many_iterations();
    test_tp_s02_max_threads();
    
    printf("\n");
    printf("================================================\n");
    printf("  内存池功能测试\n");
    printf("================================================\n");
    test_mp_f01_normal_init();
    test_mp_f02_normal_destroy();
    test_mp_f03_normal_alloc();
    test_mp_f04_normal_free();
    test_mp_f05_buffer_reuse();
    
    printf("\n");
    printf("================================================\n");
    printf("  内存池边界测试\n");
    printf("================================================\n");
    test_mp_b01_zero_buffer_size();
    test_mp_b02_zero_num_buffers();
    test_mp_b03_negative_num_buffers();
    test_mp_b04_exceed_max_buffers();
    test_mp_b05_alloc_all_buffers();
    test_mp_b06_buffer_exhausted();
    
    printf("\n");
    printf("================================================\n");
    printf("  内存池异常测试\n");
    printf("================================================\n");
    test_mp_e01_uninit_alloc();
    test_mp_e02_uninit_free();
    test_mp_e03_free_null();
    test_mp_e04_double_destroy();
    test_mp_e05_free_invalid_ptr();
    
    printf("\n");
    printf("================================================\n");
    printf("  内存池并发测试\n");
    printf("================================================\n");
    test_mp_c01_concurrent_alloc();
    test_mp_c03_alloc_free_alternate();
    
    printf("\n");
    printf("================================================\n");
    printf("  综合测试\n");
    printf("================================================\n");
    test_int_01_thread_memory_pool();
    test_int_02_multi_init_destroy();
}

int main() {
    printf("================================================\n");
    printf("  线程池与内存池测试套件\n");
    printf("  测试时间: %s %s\n", __DATE__, __TIME__);
    printf("================================================\n");
    
    run_all_tests();
    
    printf("\n");
    printf("================================================\n");
    printf("  测试结果汇总\n");
    printf("================================================\n");
    printf("  总测试数: %d\n", tests_total);
    printf("  通过: %d\n", tests_passed);
    printf("  失败: %d\n", tests_failed);
    printf("  通过率: %.1f%%\n", tests_total > 0 ? (100.0 * tests_passed / tests_total) : 0.0);
    printf("================================================\n");
    
    return tests_failed > 0 ? 1 : 0;
}