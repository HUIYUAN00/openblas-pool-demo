/**
 * @file pool.c
 * @brief OpenBLAS风格线程池和内存池实现 - 参考OpenBLAS核心优化
 */

#define _POSIX_C_SOURCE 200112L
#include "pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ===== OpenBLAS风格工作线程 ===== */

/* 线程参数结构 */
typedef struct {
    thread_pool_t *pool;
    long cpu;
} thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    thread_pool_t *pool = targ->pool;
    long cpu = targ->cpu;
    unsigned long last_tick;
    task_queue_t *queue;
    
    free(targ);  /* 释放参数结构 */
    
    while (1) {
        last_tick = rpcc();
        queue = atomic_load_ptr(&pool->thread_queue[cpu]);
        
        /* 第一阶段：自旋等待（短任务快速响应） */
        while (!queue && !pool->shutdown) {
            YIELDING;
            
            /* 超时检查 */
            if (pool->thread_timeout > 0 && 
                rpcc() - last_tick > pool->thread_timeout) {
                /* 第二阶段：休眠等待（长时间无任务） */
                pthread_mutex_lock(&pool->thread_lock[cpu]);
                pool->thread_status[cpu] = THREAD_STATUS_SLEEP;
                
                while (pool->thread_status[cpu] == THREAD_STATUS_SLEEP &&
                       !atomic_load_ptr(&pool->thread_queue[cpu]) &&
                       !pool->shutdown) {
                    pthread_cond_wait(&pool->thread_wakeup[cpu], 
                                      &pool->thread_lock[cpu]);
                }
                pthread_mutex_unlock(&pool->thread_lock[cpu]);
                last_tick = rpcc();
            }
            
            queue = atomic_load_ptr(&pool->thread_queue[cpu]);
            
            if (pool->shutdown) {
                return NULL;
            }
        }
        
        /* 第三阶段：执行任务 */
        if (pool->shutdown) {
            return NULL;
        }
        
        if (queue && queue->routine) {
            queue->routine(queue->arg, queue->assigned);
        }
        
        /* 清除队列标记完成 */
        MB;
        atomic_store_ptr(&pool->thread_queue[cpu], NULL);
    }
    
    return NULL;
}

/* ===== 线程池初始化 ===== */

int thread_pool_init(thread_pool_t *pool, int num_threads, int use_spinning) {
    if (!pool) {
        return -1;
    }
    
    if (num_threads <= 0 || num_threads > MAX_THREADS) {
        fprintf(stderr, "Warning: Invalid num_threads=%d, using default=%d\n", 
                num_threads, DEFAULT_NUM_THREADS);
        num_threads = DEFAULT_NUM_THREADS;
    }
    
    memset(pool, 0, sizeof(thread_pool_t));
    
    pool->num_threads = num_threads;
    pool->initialized = 1;
    
    /* 设置超时参数 */
    if (use_spinning) {
        pool->thread_timeout = (1 << DEFAULT_THREAD_TIMEOUT_EXP);
    } else {
        pool->thread_timeout = 0;  /* 禁用自旋 */
    }
    
    /* 初始化分发锁 */
    if (pthread_mutex_init(&pool->dispatch_lock, NULL) != 0) {
        perror("Failed to init dispatch lock");
        pool->initialized = 0;
        return -1;
    }
    
    /* 初始化每线程同步对象 */
    for (int i = 0; i < num_threads; i++) {
        atomic_store_ptr(&pool->thread_queue[i], NULL);
        pool->thread_status[i] = THREAD_STATUS_WAKEUP;
        
        if (pthread_mutex_init(&pool->thread_lock[i], NULL) != 0) {
            perror("Failed to init thread lock");
            pool->initialized = 0;
            return -1;
        }
        
        if (pthread_cond_init(&pool->thread_wakeup[i], NULL) != 0) {
            perror("Failed to init thread cond");
            pthread_mutex_destroy(&pool->thread_lock[i]);
            pool->initialized = 0;
            return -1;
        }
    }
    
    /* 创建工作线程 */
    for (int i = 0; i < num_threads; i++) {
        thread_arg_t *targ = malloc(sizeof(thread_arg_t));
        if (!targ) {
            perror("Failed to allocate thread arg");
            pool->shutdown = 1;
            for (int j = 0; j < i; j++) {
                atomic_store_ptr(&pool->thread_queue[j], (task_queue_t *)-1);
                pthread_cond_signal(&pool->thread_wakeup[j]);
            }
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            for (int j = 0; j < num_threads; j++) {
                pthread_mutex_destroy(&pool->thread_lock[j]);
                pthread_cond_destroy(&pool->thread_wakeup[j]);
            }
            pthread_mutex_destroy(&pool->dispatch_lock);
            pool->initialized = 0;
            return -1;
        }
        targ->pool = pool;
        targ->cpu = i;
        
        if (pthread_create(&pool->threads[i], NULL, worker, targ) != 0) {
            perror("Failed to create thread");
            free(targ);
            
            pool->shutdown = 1;
            for (int j = 0; j < i; j++) {
                atomic_store_ptr(&pool->thread_queue[j], (task_queue_t *)-1);
                pthread_cond_signal(&pool->thread_wakeup[j]);
            }
            
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            
            for (int j = 0; j < num_threads; j++) {
                pthread_mutex_destroy(&pool->thread_lock[j]);
                pthread_cond_destroy(&pool->thread_wakeup[j]);
            }
            pthread_mutex_destroy(&pool->dispatch_lock);
            pool->initialized = 0;
            return -1;
        }
    }
    
    return 0;
}

void thread_pool_shutdown(thread_pool_t *pool) {
    if (!pool || !pool->initialized) return;
    
    pool->shutdown = 1;
    MB;
    
    /* 唤醒所有线程 */
    for (int i = 0; i < pool->num_threads; i++) {
        atomic_store_ptr(&pool->thread_queue[i], (task_queue_t *)-1);
        
        pthread_mutex_lock(&pool->thread_lock[i]);
        if (pool->thread_status[i] == THREAD_STATUS_SLEEP) {
            pool->thread_status[i] = THREAD_STATUS_WAKEUP;
            pthread_cond_signal(&pool->thread_wakeup[i]);
        }
        pthread_mutex_unlock(&pool->thread_lock[i]);
    }
    
    /* 等待线程退出 */
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    /* 销毁同步对象 */
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_mutex_destroy(&pool->thread_lock[i]);
        pthread_cond_destroy(&pool->thread_wakeup[i]);
    }
    pthread_mutex_destroy(&pool->dispatch_lock);
    
    pool->initialized = 0;
}

/* ===== OpenBLAS风格任务分发 ===== */

int thread_pool_parallel_for(thread_pool_t *pool, 
                             task_func_t func, 
                             void *arg, 
                             int num_tasks) {
    if (!pool || !pool->initialized || !func || num_tasks <= 0) {
        return -1;
    }
    
    /* 构建任务队列 */
    task_queue_t *tasks = malloc(sizeof(task_queue_t) * num_tasks);
    if (!tasks) {
        perror("Failed to allocate task queue");
        return -1;
    }
    
    for (int i = 0; i < num_tasks; i++) {
        tasks[i].routine = func;
        tasks[i].arg = arg;
        tasks[i].assigned = i;
        tasks[i].next = (i < num_tasks - 1) ? &tasks[i + 1] : NULL;
    }
    
    pthread_mutex_lock(&pool->dispatch_lock);
    
    /* 分发任务到线程 */
    int threads_used = (num_tasks < pool->num_threads) ? num_tasks : pool->num_threads;
    
    for (int i = 0; i < threads_used; i++) {
        tasks[i].assigned = i;
        MB;
        atomic_store_ptr(&pool->thread_queue[i], &tasks[i]);
        
        /* 唤醒休眠线程 */
        pthread_mutex_lock(&pool->thread_lock[i]);
        if (pool->thread_status[i] == THREAD_STATUS_SLEEP) {
            pool->thread_status[i] = THREAD_STATUS_WAKEUP;
            pthread_cond_signal(&pool->thread_wakeup[i]);
        }
        pthread_mutex_unlock(&pool->thread_lock[i]);
    }
    
    pthread_mutex_unlock(&pool->dispatch_lock);
    
    /* 主线程等待所有任务完成（自旋等待） */
    for (int i = 0; i < threads_used; i++) {
        while (atomic_load_ptr(&pool->thread_queue[i])) {
            YIELDING;
        }
    }
    
    MB;  /* 确保结果可见 */
    
    free(tasks);
    
    return 0;
}

/* ===== TLS内存池辅助函数 ===== */

static void tls_destructor(void *ptr) {
    if (ptr) {
        alloc_header_t **table = (alloc_header_t **)ptr;
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (table[i]) {
                free(table[i]);
                table[i] = NULL;
            }
        }
        free(table);
    }
}

static alloc_header_t **get_tls_table(memory_pool_t *pool) {
    if (!pool->tls_key) {
        pthread_mutex_lock(&pool->init_lock);
        if (!pool->tls_key) {
            pthread_key_create(&pool->tls_key, tls_destructor);
        }
        pthread_mutex_unlock(&pool->init_lock);
    }
    
    alloc_header_t **table = (alloc_header_t **)pthread_getspecific(pool->tls_key);
    
    if (!table) {
        table = (alloc_header_t **)malloc(sizeof(alloc_header_t *) * MAX_BUFFERS);
        if (!table) {
            perror("Failed to allocate TLS table");
            return NULL;
        }
        memset(table, 0, sizeof(alloc_header_t *) * MAX_BUFFERS);
        pthread_setspecific(pool->tls_key, table);
    }
    
    return table;
}

/* ===== 内存池初始化 ===== */

int memory_pool_init(memory_pool_t *pool, 
                     size_t buffer_size, 
                     int num_buffers, 
                     int use_tls) {
    if (!pool) {
        return -1;
    }
    
    if (buffer_size == 0) {
        buffer_size = DEFAULT_BUFFER_SIZE;
    }
    
    if (num_buffers <= 0 || num_buffers > MAX_BUFFERS) {
        fprintf(stderr, "Warning: Invalid num_buffers=%d, using default=%d\n", 
                num_buffers, DEFAULT_NUM_BUFFERS);
        num_buffers = DEFAULT_NUM_BUFFERS;
    }
    
    memset(pool, 0, sizeof(memory_pool_t));
    
    pool->buffer_size = buffer_size + sizeof(alloc_header_t);
    pool->num_buffers = num_buffers;
    pool->initialized = 1;
    pool->use_prealloc = !use_tls;
    
    if (pthread_mutex_init(&pool->init_lock, NULL) != 0) {
        perror("Failed to init mutex");
        pool->initialized = 0;
        return -1;
    }
    
    /* 预分配模式 */
    if (pool->use_prealloc) {
        for (int i = 0; i < num_buffers; i++) {
            int ret = posix_memalign(&pool->prealloc_buffers[i], 64, pool->buffer_size);
            if (ret != 0 || !pool->prealloc_buffers[i]) {
                fprintf(stderr, "Error: Failed to allocate aligned buffer %d (size=%zu, align=64, err=%d)\n", 
                        i, pool->buffer_size, ret);
                
                for (int j = 0; j < i; j++) {
                    free(pool->prealloc_buffers[j]);
                    pool->prealloc_buffers[j] = NULL;
                }
                
                pthread_mutex_destroy(&pool->init_lock);
                pool->initialized = 0;
                return -1;
            }
            pool->prealloc_used[i] = 0;
        }
    }
    
    return 0;
}

void memory_pool_destroy(memory_pool_t *pool) {
    if (!pool || !pool->initialized) return;
    
    /* 预分配模式：释放所有缓冲区 */
    if (pool->use_prealloc) {
        for (int i = 0; i < pool->num_buffers; i++) {
            if (pool->prealloc_buffers[i]) {
                free(pool->prealloc_buffers[i]);
                pool->prealloc_buffers[i] = NULL;
            }
        }
    }
    
    /* TLS模式：pthread_key会自动调用tls_destructor */
    if (pool->tls_key) {
        pthread_key_delete(pool->tls_key);
        pool->tls_key = 0;
    }
    
    pthread_mutex_destroy(&pool->init_lock);
    
    pool->initialized = 0;
}

/* ===== 内存分配 ===== */

void *memory_alloc(memory_pool_t *pool) {
    if (!pool || !pool->initialized) {
        return NULL;
    }
    
    /* 预分配模式（全局池） */
    if (pool->use_prealloc) {
        pthread_mutex_lock(&pool->init_lock);
        
        void *result = NULL;
        for (int i = 0; i < pool->num_buffers; i++) {
            if (!pool->prealloc_used[i]) {
                pool->prealloc_used[i] = 1;
                alloc_header_t *header = (alloc_header_t *)pool->prealloc_buffers[i];
                header->used = 1;
                result = (void *)((char *)header + sizeof(alloc_header_t));
                break;
            }
        }
        
        pthread_mutex_unlock(&pool->init_lock);
        
        if (!result) {
            fprintf(stderr, "Warning: No free buffers available (pool size=%d)\n", 
                    pool->num_buffers);
        }
        
        return result;
    }
    
    /* TLS模式（每线程独立） */
    alloc_header_t **table = get_tls_table(pool);
    if (!table) {
        return NULL;
    }
    
    /* 查找空闲槽位 */
    int position = 0;
    while (position < pool->num_buffers) {
        if (!table[position] || !table[position]->used) {
            break;
        }
        position++;
    }
    
    if (position >= pool->num_buffers) {
        fprintf(stderr, "Warning: TLS memory pool exhausted (max=%d)\n", pool->num_buffers);
        return NULL;
    }
    
    /* 惰性分配：首次使用时才分配 */
    alloc_header_t *header = table[position];
    if (!header) {
        int ret = posix_memalign((void **)&header, 64, pool->buffer_size);
        if (ret != 0 || !header) {
            perror("Failed to allocate TLS buffer");
            return NULL;
        }
        table[position] = header;
    }
    
    header->used = 1;
    
    /* 返回实际数据区域（跳过alloc_header_t） */
    return (void *)((char *)header + sizeof(alloc_header_t));
}

/* ===== 内存释放（惰性） ===== */

void memory_free(memory_pool_t *pool, void *buffer) {
    if (!pool || !pool->initialized || !buffer) return;
    
    /* 获取实际分配结构（回退指针） */
    alloc_header_t *header = (void *)((char *)buffer - sizeof(alloc_header_t));
    
    /* 仅标记为未使用（不真正释放） */
    header->used = 0;
}