/**
 * @file pool.h
 * @brief OpenBLAS 风格线程池和内存池
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

typedef void (*task_func_t)(void *arg, int thread_id);

typedef struct thread_pool {
    pthread_t threads[MAX_THREADS];
    int num_threads;
    int initialized;
    
    pthread_mutex_t lock;
    pthread_cond_t work_cond;
    pthread_cond_t done_cond;
    
    task_func_t current_func;
    void *current_arg;
    int num_tasks;
    int tasks_assigned;
    int tasks_completed;
    int shutdown;
} thread_pool_t;

typedef struct memory_pool {
    pthread_mutex_t lock;
    void *buffers[MAX_BUFFERS];
    int buffer_used[MAX_BUFFERS];
    size_t buffer_size;
    int num_buffers;
    int initialized;
} memory_pool_t;

/**
 * @brief 初始化线程池
 * @param pool 线程池结构体指针
 * @param num_threads 线程数量（范围：1-MAX_THREADS，若无效则使用默认值）
 * @return 0成功，-1失败
 */
int thread_pool_init(thread_pool_t *pool, int num_threads);

/**
 * @brief 关闭并销毁线程池
 * @param pool 线程池结构体指针
 */
void thread_pool_shutdown(thread_pool_t *pool);

/**
 * @brief 并行执行任务
 * @param pool 线程池结构体指针
 * @param func 任务函数指针
 * @param arg 传递给任务函数的参数
 * @param num_tasks 任务总数
 * @return 0成功，-1失败
 */
int thread_pool_parallel_for(thread_pool_t *pool, task_func_t func, void *arg, int num_tasks);

/**
 * @brief 初始化内存池
 * @param pool 内存池结构体指针
 * @param buffer_size 每个缓冲区大小（字节，若为0则使用默认值64KB）
 * @param num_buffers 缓冲区数量（范围：1-MAX_BUFFERS，若无效则使用默认值）
 * @return 0成功，-1失败（内存分配失败）
 */
int memory_pool_init(memory_pool_t *pool, size_t buffer_size, int num_buffers);

/**
 * @brief 销毁内存池
 * @param pool 内存池结构体指针
 */
void memory_pool_destroy(memory_pool_t *pool);

/**
 * @brief 从内存池分配缓冲区
 * @param pool 内存池结构体指针
 * @return 成功返回缓冲区指针，失败返回NULL
 */
void *memory_alloc(memory_pool_t *pool);

/**
 * @brief 释放缓冲区到内存池
 * @param pool 内存池结构体指针
 * @param buffer 要释放的缓冲区指针
 */
void memory_free(memory_pool_t *pool, void *buffer);

#endif