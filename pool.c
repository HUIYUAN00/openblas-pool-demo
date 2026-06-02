/**
 * @file pool.c
 * @brief OpenBLAS风格线程池和内存池实现 - 内部细节隐藏
 */

#define _POSIX_C_SOURCE 200112L
#include "pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* ===== 内部宏定义（平台相关） ===== */

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

#define atomic_load_ptr(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define atomic_store_ptr(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

#define THREAD_STATUS_WAKEUP  4
#define THREAD_STATUS_SLEEP   2
#define DEFAULT_THREAD_TIMEOUT_EXP 24

/* ===== 内部结构体定义 ===== */

typedef struct task_queue {
    task_func_t routine;
    void *arg;
    int assigned;
    struct task_queue *next;
} task_queue_t;

typedef struct alloc_header {
    int used;
    int attr;
    void (*release_func)(struct alloc_header *);
    char pad[52];
} __attribute__((aligned(64))) alloc_header_t;

struct thread_pool {
    pthread_t threads[MAX_THREADS];
    int num_threads;
    int initialized;
    
    pthread_mutex_t thread_lock[MAX_THREADS];
    pthread_cond_t thread_wakeup[MAX_THREADS];
    volatile long thread_status[MAX_THREADS];
    task_queue_t *volatile thread_queue[MAX_THREADS] __attribute__((aligned(128)));
    
    pthread_mutex_t dispatch_lock;
    unsigned int thread_timeout;
    int shutdown;
};

struct memory_pool {
    pthread_key_t tls_key;
    pthread_mutex_t init_lock;
    
    size_t buffer_size;
    size_t total_buffer_size;
    int num_buffers;
    int initialized;
    
    int use_prealloc;
    void *prealloc_buffers[MAX_BUFFERS];
    int prealloc_used[MAX_BUFFERS];
};

/* ===== 内部辅助函数 ===== */

static inline unsigned long rpcc(void) {
#if defined(__aarch64__)
    unsigned long val;
    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#elif defined(__x86_64__)
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((unsigned long)hi << 32) | lo;
#else
    return 0;
#endif
}

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
    
    free(targ);
    
    while (1) {
        last_tick = rpcc();
        queue = atomic_load_ptr(&pool->thread_queue[cpu]);
        
        while (!queue && !pool->shutdown) {
            YIELDING;
            
            if (pool->thread_timeout > 0) {
                unsigned long elapsed = rpcc() - last_tick;
                if (elapsed > pool->thread_timeout && elapsed < (1UL << 63)) {
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
            }
            
            queue = atomic_load_ptr(&pool->thread_queue[cpu]);
            
            if (pool->shutdown) {
                return NULL;
            }
        }
        
        if (pool->shutdown) {
            return NULL;
        }
        
        if (queue && queue->routine) {
            queue->routine(queue->arg, queue->assigned);
        }
        
        MB;
        atomic_store_ptr(&pool->thread_queue[cpu], NULL);
    }
    
    return NULL;
}

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

/* ===== Thread Pool API实现 ===== */

thread_pool_t *thread_pool_create(int num_threads, int use_spinning) {
    thread_pool_t *pool = malloc(sizeof(thread_pool_t));
    if (!pool) {
        perror("Failed to allocate thread pool");
        return NULL;
    }
    
    if (num_threads <= 0 || num_threads > MAX_THREADS) {
        fprintf(stderr, "Warning: Invalid num_threads=%d, using default=%d\n", 
                num_threads, DEFAULT_NUM_THREADS);
        num_threads = DEFAULT_NUM_THREADS;
    }
    
    memset(pool, 0, sizeof(thread_pool_t));
    
    pool->num_threads = num_threads;
    pool->initialized = 1;
    
    if (use_spinning) {
        pool->thread_timeout = (1 << DEFAULT_THREAD_TIMEOUT_EXP);
    } else {
        pool->thread_timeout = 0;
    }
    
    if (pthread_mutex_init(&pool->dispatch_lock, NULL) != 0) {
        perror("Failed to init dispatch lock");
        free(pool);
        return NULL;
    }
    
    for (int i = 0; i < num_threads; i++) {
        atomic_store_ptr(&pool->thread_queue[i], NULL);
        pool->thread_status[i] = THREAD_STATUS_WAKEUP;
        
        if (pthread_mutex_init(&pool->thread_lock[i], NULL) != 0) {
            perror("Failed to init thread lock");
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&pool->thread_lock[j]);
                pthread_cond_destroy(&pool->thread_wakeup[j]);
            }
            pthread_mutex_destroy(&pool->dispatch_lock);
            free(pool);
            return NULL;
        }
        
        if (pthread_cond_init(&pool->thread_wakeup[i], NULL) != 0) {
            perror("Failed to init thread cond");
            pthread_mutex_destroy(&pool->thread_lock[i]);
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&pool->thread_lock[j]);
                pthread_cond_destroy(&pool->thread_wakeup[j]);
            }
            pthread_mutex_destroy(&pool->dispatch_lock);
            free(pool);
            return NULL;
        }
    }
    
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
            free(pool);
            return NULL;
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
            free(pool);
            return NULL;
        }
    }
    
    return pool;
}

void thread_pool_destroy(thread_pool_t *pool) {
    if (!pool || !pool->initialized) return;
    
    pool->shutdown = 1;
    MB;
    
    for (int i = 0; i < pool->num_threads; i++) {
        atomic_store_ptr(&pool->thread_queue[i], (task_queue_t *)-1);
        
        pthread_mutex_lock(&pool->thread_lock[i]);
        if (pool->thread_status[i] == THREAD_STATUS_SLEEP) {
            pool->thread_status[i] = THREAD_STATUS_WAKEUP;
            pthread_cond_signal(&pool->thread_wakeup[i]);
        }
        pthread_mutex_unlock(&pool->thread_lock[i]);
    }
    
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_mutex_destroy(&pool->thread_lock[i]);
        pthread_cond_destroy(&pool->thread_wakeup[i]);
    }
    pthread_mutex_destroy(&pool->dispatch_lock);
    
    pool->initialized = 0;
    free(pool);
}

int thread_pool_parallel_for(thread_pool_t *pool, 
                              task_func_t func, 
                              void *arg, 
                              int num_tasks) {
    if (!pool || !pool->initialized || !func || num_tasks <= 0) {
        return -1;
    }
    
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
    
    int threads_used = (num_tasks < pool->num_threads) ? num_tasks : pool->num_threads;
    
    for (int i = 0; i < threads_used; i++) {
        tasks[i].assigned = i;
        MB;
        atomic_store_ptr(&pool->thread_queue[i], &tasks[i]);
        
        pthread_mutex_lock(&pool->thread_lock[i]);
        if (pool->thread_status[i] == THREAD_STATUS_SLEEP) {
            pool->thread_status[i] = THREAD_STATUS_WAKEUP;
            pthread_cond_signal(&pool->thread_wakeup[i]);
        }
        pthread_mutex_unlock(&pool->thread_lock[i]);
    }
    
    pthread_mutex_unlock(&pool->dispatch_lock);
    
    for (int i = 0; i < threads_used; i++) {
        int retry = 0;
        while (atomic_load_ptr(&pool->thread_queue[i]) && retry < 100000) {
            YIELDING;
            retry++;
        }
        if (retry >= 100000) {
            fprintf(stderr, "Warning: Task %d timeout after %d retries\n", i, retry);
        }
    }
    
    MB;
    
    free(tasks);
    
    return 0;
}

/* ===== Thread Pool Getters实现 ===== */

int thread_pool_get_num_threads(thread_pool_t *pool) {
    if (!pool) return 0;
    return pool->num_threads;
}

unsigned int thread_pool_get_timeout(thread_pool_t *pool) {
    if (!pool) return 0;
    return pool->thread_timeout;
}

int thread_pool_is_initialized(thread_pool_t *pool) {
    if (!pool) return 0;
    return pool->initialized;
}

/* ===== Memory Pool API实现 ===== */

memory_pool_t *memory_pool_create(size_t buffer_size, int num_buffers, int use_tls) {
    memory_pool_t *pool = malloc(sizeof(memory_pool_t));
    if (!pool) {
        perror("Failed to allocate memory pool");
        return NULL;
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
    pool->total_buffer_size = buffer_size + sizeof(alloc_header_t);
    pool->num_buffers = num_buffers;
    pool->initialized = 1;
    pool->use_prealloc = !use_tls;
    
    if (pthread_mutex_init(&pool->init_lock, NULL) != 0) {
        perror("Failed to init mutex");
        free(pool);
        return NULL;
    }
    
    if (pool->use_prealloc) {
        for (int i = 0; i < num_buffers; i++) {
            void *tmp;
            int ret = posix_memalign(&tmp, 64, pool->total_buffer_size);
            if (ret != 0 || !tmp) {
                fprintf(stderr, "Error: Failed to allocate aligned buffer %d (size=%zu, align=64, err=%d)\n", 
                        i, pool->total_buffer_size, ret);
                
                for (int j = 0; j < i; j++) {
                    free(pool->prealloc_buffers[j]);
                    pool->prealloc_buffers[j] = NULL;
                }
                
                pthread_mutex_destroy(&pool->init_lock);
                free(pool);
                return NULL;
            }
            pool->prealloc_buffers[i] = tmp;
            pool->prealloc_used[i] = 0;
        }
    }
    
    return pool;
}

void memory_pool_destroy(memory_pool_t *pool) {
    if (!pool || !pool->initialized) return;
    
    if (pool->use_prealloc) {
        for (int i = 0; i < pool->num_buffers; i++) {
            if (pool->prealloc_buffers[i]) {
                free(pool->prealloc_buffers[i]);
                pool->prealloc_buffers[i] = NULL;
            }
        }
    }
    
    if (pool->tls_key) {
        pthread_key_delete(pool->tls_key);
        pool->tls_key = 0;
    }
    
    pthread_mutex_destroy(&pool->init_lock);
    
    pool->initialized = 0;
    free(pool);
}

void *memory_alloc(memory_pool_t *pool) {
    if (!pool || !pool->initialized) {
        return NULL;
    }
    
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
    
    alloc_header_t **table = get_tls_table(pool);
    if (!table) {
        return NULL;
    }
    
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
    
    alloc_header_t *header = table[position];
    if (!header) {
        void *tmp;
        int ret = posix_memalign(&tmp, 64, pool->total_buffer_size);
        if (ret != 0 || !tmp) {
            perror("Failed to allocate TLS buffer");
            return NULL;
        }
        header = (alloc_header_t *)tmp;
        table[position] = header;
    }
    
    header->used = 1;
    
    return (void *)((char *)header + sizeof(alloc_header_t));
}

void memory_free(memory_pool_t *pool, void *buffer) {
    if (!pool || !pool->initialized || !buffer) return;
    
    alloc_header_t *header = (void *)((char *)buffer - sizeof(alloc_header_t));
    
    header->used = 0;
}

/* ===== Memory Pool Getters实现 ===== */

size_t memory_pool_get_buffer_size(memory_pool_t *pool) {
    if (!pool) return 0;
    return pool->buffer_size;
}

size_t memory_pool_get_total_buffer_size(memory_pool_t *pool) {
    if (!pool) return 0;
    return pool->total_buffer_size;
}

int memory_pool_get_num_buffers(memory_pool_t *pool) {
    if (!pool) return 0;
    return pool->num_buffers;
}

int memory_pool_is_tls_enabled(memory_pool_t *pool) {
    if (!pool) return 0;
    return !pool->use_prealloc;
}

int memory_pool_is_initialized(memory_pool_t *pool) {
    if (!pool) return 0;
    return pool->initialized;
}