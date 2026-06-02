/**
 * @file pool.h
 * @brief OpenBLAS风格线程池和内存池 - 不透明接口
 * 
 * 所有实现细节隐藏在接缝之后，调用者通过API函数和getter访问。
 */

#ifndef POOL_H
#define POOL_H

#include <stdint.h>
#include <stddef.h>

/* ===== 不透明类型声明 ===== */

typedef struct thread_pool thread_pool_t;
typedef struct memory_pool memory_pool_t;

/* ===== 任务函数类型 ===== */

typedef void (*task_func_t)(void *arg, int thread_id);

/* ===== 常量定义 ===== */

#define MAX_THREADS 64
#define MAX_BUFFERS 16
#define DEFAULT_BUFFER_SIZE (64 * 1024)
#define DEFAULT_NUM_THREADS 4
#define DEFAULT_NUM_BUFFERS 8

/* ===== Thread Pool API ===== */

/**
 * @brief 创建线程池（OpenBLAS风格优化）
 * @param num_threads 线程数量（范围：1-MAX_THREADS，若无效则使用默认值）
 * @param use_spinning 是否启用自旋等待优化（0=禁用，1=启用）
 * @return 成功返回线程池指针，失败返回NULL
 */
thread_pool_t *thread_pool_create(int num_threads, int use_spinning);

/**
 * @brief 销毁线程池
 * @param pool 线程池指针
 */
void thread_pool_destroy(thread_pool_t *pool);

/**
 * @brief 并行执行任务（OpenBLAS风格原子队列分发）
 * @param pool 线程池指针
 * @param func 任务函数指针
 * @param arg 传递给任务函数的参数
 * @param num_tasks 任务总数
 * @return 0成功，-1失败
 */
int thread_pool_parallel_for(thread_pool_t *pool, task_func_t func, void *arg, int num_tasks);

/* ===== Thread Pool Getters ===== */

/**
 * @brief 获取线程数量
 * @param pool 线程池指针
 * @return 线程数量
 */
int thread_pool_get_num_threads(thread_pool_t *pool);

/**
 * @brief 获取超时参数（时钟周期数）
 * @param pool 线程池指针
 * @return 超时参数，0表示禁用自旋
 */
unsigned int thread_pool_get_timeout(thread_pool_t *pool);

/**
 * @brief 检查是否已初始化
 * @param pool 线程池指针
 * @return 1已初始化，0未初始化
 */
int thread_pool_is_initialized(thread_pool_t *pool);

/* ===== Memory Pool API ===== */

/**
 * @brief 创建内存池（OpenBLAS风格TLS + 惰性分配）
 * @param buffer_size 每个缓冲区的用户可用大小（字节，若为0则使用默认值64KB）
 * @param num_buffers 缓冲区数量（范围：1-MAX_BUFFERS，若无效则使用默认值）
 * @param use_tls 是否启用TLS优化（0=全局池，1=线程本地）
 * @return 成功返回内存池指针，失败返回NULL
 */
memory_pool_t *memory_pool_create(size_t buffer_size, int num_buffers, int use_tls);

/**
 * @brief 销毁内存池
 * @param pool 内存池指针
 */
void memory_pool_destroy(memory_pool_t *pool);

/**
 * @brief 从内存池分配缓冲区（TLS优化）
 * @param pool 内存池指针
 * @return 成功返回缓冲区指针，失败返回NULL
 */
void *memory_alloc(memory_pool_t *pool);

/**
 * @brief 释放缓冲区到内存池（惰性释放）
 * @param pool 内存池指针
 * @param buffer 要释放的缓冲区指针
 */
void memory_free(memory_pool_t *pool, void *buffer);

/* ===== Memory Pool Getters ===== */

/**
 * @brief 获取用户可用缓冲区大小
 * @param pool 内存池指针
 * @return 用户可用字节数（不含头部开销）
 */
size_t memory_pool_get_buffer_size(memory_pool_t *pool);

/**
 * @brief 获取实际分配的缓冲区大小（含头部）
 * @param pool 内存池指针
 * @return 实际分配字节数
 */
size_t memory_pool_get_total_buffer_size(memory_pool_t *pool);

/**
 * @brief 获取缓冲区数量
 * @param pool 内存池指针
 * @return 缓冲区数量
 */
int memory_pool_get_num_buffers(memory_pool_t *pool);

/**
 * @brief 检查是否启用TLS模式
 * @param pool 内存池指针
 * @return 1启用TLS，0使用预分配全局池
 */
int memory_pool_is_tls_enabled(memory_pool_t *pool);

/**
 * @brief 检查是否已初始化
 * @param pool 内存池指针
 * @return 1已初始化，0未初始化
 */
int memory_pool_is_initialized(memory_pool_t *pool);

#endif