/**
 * @file pool.c
 * @brief OpenBLAS风格线程池和内存池实现
 */

#include "pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void *worker(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    
    while (1) {
        pthread_mutex_lock(&pool->lock);
        
        while (!pool->shutdown && pool->tasks_assigned >= pool->num_tasks) {
            pthread_cond_wait(&pool->work_cond, &pool->lock);
        }
        
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->lock);
            pthread_exit(NULL);
        }
        
        int my_task_id = pool->tasks_assigned;
        pool->tasks_assigned++;
        
        task_func_t func = pool->current_func;
        void *arg_data = pool->current_arg;
        
        pthread_mutex_unlock(&pool->lock);
        
        if (func) {
            func(arg_data, my_task_id);
        }
        
        pthread_mutex_lock(&pool->lock);
        pool->tasks_completed++;
        
        if (pool->tasks_completed == pool->num_tasks) {
            pthread_cond_signal(&pool->done_cond);
        }
        
        pthread_mutex_unlock(&pool->lock);
    }
    
    return NULL;
}

int thread_pool_init(thread_pool_t *pool, int num_threads) {
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
    
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        perror("Failed to init mutex");
        pool->initialized = 0;
        return -1;
    }
    
    if (pthread_cond_init(&pool->work_cond, NULL) != 0) {
        perror("Failed to init work condition");
        pthread_mutex_destroy(&pool->lock);
        pool->initialized = 0;
        return -1;
    }
    
    if (pthread_cond_init(&pool->done_cond, NULL) != 0) {
        perror("Failed to init done condition");
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->work_cond);
        pool->initialized = 0;
        return -1;
    }
    
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker, pool) != 0) {
            perror("Failed to create thread");
            
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work_cond);
            
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            
            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->work_cond);
            pthread_cond_destroy(&pool->done_cond);
            pool->initialized = 0;
            return -1;
        }
    }
    
    return 0;
}

void thread_pool_shutdown(thread_pool_t *pool) {
    if (!pool || !pool->initialized) return;
    
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->lock);
    
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->work_cond);
    pthread_cond_destroy(&pool->done_cond);
    
    pool->initialized = 0;
}

int thread_pool_parallel_for(thread_pool_t *pool, task_func_t func, void *arg, int num_tasks) {
    if (!pool || !pool->initialized || !func) {
        return -1;
    }
    
    if (num_tasks <= 0) {
        return -1;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    pool->current_func = func;
    pool->current_arg = arg;
    pool->num_tasks = num_tasks;
    pool->tasks_assigned = 0;
    pool->tasks_completed = 0;
    
    pthread_cond_broadcast(&pool->work_cond);
    
    while (pool->tasks_completed < pool->num_tasks) {
        pthread_cond_wait(&pool->done_cond, &pool->lock);
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    return 0;
}

int memory_pool_init(memory_pool_t *pool, size_t buffer_size, int num_buffers) {
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
    
    pool->buffer_size = buffer_size;
    pool->num_buffers = num_buffers;
    pool->initialized = 1;
    
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        perror("Failed to init mutex");
        pool->initialized = 0;
        return -1;
    }
    
    for (int i = 0; i < num_buffers; i++) {
        pool->buffers[i] = malloc(buffer_size);
        if (!pool->buffers[i]) {
            fprintf(stderr, "Error: Failed to allocate buffer %d (size=%zu)\n", 
                    i, buffer_size);
            
            for (int j = 0; j < i; j++) {
                free(pool->buffers[j]);
                pool->buffers[j] = NULL;
            }
            
            pthread_mutex_destroy(&pool->lock);
            pool->initialized = 0;
            return -1;
        }
        pool->buffer_used[i] = 0;
    }
    
    return 0;
}

void memory_pool_destroy(memory_pool_t *pool) {
    if (!pool || !pool->initialized) return;
    
    pthread_mutex_lock(&pool->lock);
    
    for (int i = 0; i < pool->num_buffers; i++) {
        if (pool->buffers[i]) {
            free(pool->buffers[i]);
            pool->buffers[i] = NULL;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    
    pool->initialized = 0;
}

void *memory_alloc(memory_pool_t *pool) {
    if (!pool || !pool->initialized) {
        return NULL;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    void *result = NULL;
    for (int i = 0; i < pool->num_buffers; i++) {
        if (!pool->buffer_used[i]) {
            pool->buffer_used[i] = 1;
            result = pool->buffers[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    if (!result) {
        fprintf(stderr, "Warning: No free buffers available (pool size=%d)\n", 
                pool->num_buffers);
    }
    
    return result;
}

void memory_free(memory_pool_t *pool, void *buffer) {
    if (!pool || !pool->initialized || !buffer) return;
    
    pthread_mutex_lock(&pool->lock);
    
    for (int i = 0; i < pool->num_buffers; i++) {
        if (pool->buffers[i] == buffer) {
            pool->buffer_used[i] = 0;
            break;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
}