/**
 * @file pool.h
 * @brief OpenBLAS 风格线程池和内存池 - 参考OpenBLAS核心优化
 */

#ifndef POOL_H
#define POOL_H

#include <pthread.h>
#include <stdint.h>

#define MAX_THREADS 64
#define MAX_BUFFERS 16
#define DEFAULT_BUFFER_SIZE (64 * 1024)
#define DEFAULT_NUM_THREADS 4
#define DEFAULT_NUM_BUFFERS 8

/* ===== OpenBLAS风格优化 ===== */

/* 内存屏障和自旋等待指令 */
#if defined(__aarch64__)
#define MB   __asm__ __volatile__ ("dmb sy" ::: "memory")
#define WMB  __asm__ __volatile__ ("dmb st" ::: "memory")
#define RMB  __asm__ __volatile__ ("dmb ld" ::: "memory")
#define YIELDING __asm__ __volatile__ ("yield" ::: "memory")
#elif defined(__x86_64__)
#define MB   __asm__ __volatile__ ("mfence" ::: "memory")
#define WMB  __asm__ __volatile__ ("sfence" ::: "memory")
#define RMB  __asm__ __volatile__ ("lfence" ::: "memory")
#define YIELDING __asm__ __volatile__ ("pause" ::: "memory")
#else
#define MB   __sync_synchronize()
#define WMB  __sync_synchronize()
#define RMB  __sync_synchronize()
#define YIELDING (void)0
#endif

/* 原子操作 - C11原子内置函数 */
#define atomic_load_ptr(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define atomic_store_ptr(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* 线程状态常量 */
#define THREAD_STATUS_WAKEUP  4
#define THREAD_STATUS_SLEEP   2

/* 超时参数：默认2^24个时钟周期（约几十毫秒） */
#define DEFAULT_THREAD_TIMEOUT_EXP 24

typedef void (*task_func_t)(void *arg, int thread_id);

/* 任务队列结构 - 参考OpenBLAS blas_queue_t */
typedef struct task_queue {
    task_func_t routine;
    void *arg;
    int assigned;
    struct task_queue *next;
} task_queue_t;

typedef struct thread_pool {
    pthread_t threads[MAX_THREADS];
    int num_threads;
    int initialized;
    
    /* OpenBLAS风格：每线程独立状态 */
    pthread_mutex_t thread_lock[MAX_THREADS];
    pthread_cond_t thread_wakeup[MAX_THREADS];
    volatile long thread_status[MAX_THREADS];
    task_queue_t *volatile thread_queue[MAX_THREADS] __attribute__((aligned(128)));
    
    /* 任务分发锁 */
    pthread_mutex_t dispatch_lock;
    
    /* 超时参数 */
    unsigned int thread_timeout;
    
    int shutdown;
} thread_pool_t;

/* OpenBLAS风格：alloc_t头部结构 */
typedef struct alloc_header {
    int used;
    int attr;
    void (*release_func)(struct alloc_header *);
    char pad[64 - 2 * sizeof(int) - sizeof(void(*)(struct alloc_header *))];
} alloc_header_t;

typedef struct memory_pool {
    /* TLS支持：线程本地存储key */
    pthread_key_t tls_key;
    pthread_mutex_t init_lock;
    
    /* 全局参数 */
    size_t buffer_size;
    int num_buffers;
    int initialized;
    
    /* 预分配策略（可选） */
    int use_prealloc;
    void *prealloc_buffers[MAX_BUFFERS];
    int prealloc_used[MAX_BUFFERS];
} memory_pool_t;

/**
 * @brief 初始化线程池（OpenBLAS风格优化）
 * @param pool 线程池结构体指针
 * @param num_threads 线程数量（范围：1-MAX_THREADS，若无效则使用默认值）
 * @param use_spinning 是否启用自旋等待优化（0=禁用，1=启用）
 * @return 0成功，-1失败
 */
int thread_pool_init(thread_pool_t *pool, int num_threads, int use_spinning);

/**
 * @brief 关闭并销毁线程池
 * @param pool 线程池结构体指针
 */
void thread_pool_shutdown(thread_pool_t *pool);

/**
 * @brief 并行执行任务（OpenBLAS风格原子队列分发）
 * @param pool 线程池结构体指针
 * @param func 任务函数指针
 * @param arg 传递给任务函数的参数
 * @param num_tasks 任务总数
 * @return 0成功，-1失败
 */
int thread_pool_parallel_for(thread_pool_t *pool, task_func_t func, void *arg, int num_tasks);

/**
 * @brief 初始化内存池（OpenBLAS风格TLS + 惰性分配）
 * @param pool 内存池结构体指针
 * @param buffer_size 每个缓冲区大小（字节，若为0则使用默认值64KB）
 * @param num_buffers 缓冲区数量（范围：1-MAX_BUFFERS，若无效则使用默认值）
 * @param use_tls 是否启用TLS优化（0=全局池，1=线程本地）
 * @return 0成功，-1失败（内存分配失败）
 */
int memory_pool_init(memory_pool_t *pool, size_t buffer_size, int num_buffers, int use_tls);

/**
 * @brief 销毁内存池
 * @param pool 内存池结构体指针
 */
void memory_pool_destroy(memory_pool_t *pool);

/**
 * @brief 从内存池分配缓冲区（TLS优化）
 * @param pool 内存池结构体指针
 * @return 成功返回缓冲区指针，失败返回NULL
 */
void *memory_alloc(memory_pool_t *pool);

/**
 * @brief 释放缓冲区到内存池（惰性释放）
 * @param pool 内存池结构体指针
 * @param buffer 要释放的缓冲区指针
 */
void memory_free(memory_pool_t *pool, void *buffer);

/**
 * @brief 读取时间戳计数器（用于超时检测）
 * @return 当前时钟周期数
 */
static inline unsigned long rpcc(void) {
#if defined(__aarch64__)
    unsigned long val;
    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#elif defined(__x86_64__)
    unsigned long val;
    __asm__ __volatile__ ("rdtsc" : "=A" (val));
    return val;
#else
    return 0;
#endif
}

#endif