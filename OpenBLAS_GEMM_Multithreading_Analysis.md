# OpenBLAS GEMM 多线程实现深度分析

> **文档版本**: 1.0  
> **分析日期**: 2026-04-17  
> **适用版本**: OpenBLAS 0.3.32.dev  
> **重点架构**: ARM64 (Neoverse V1/V2, A64FX等)

---

## 一、整体架构

### 1.1 多线程调用链

```
┌─────────────────────────────────────────────────────────────┐
│                    GEMM 多线程调用链                          │
├─────────────────────────────────────────────────────────────┤
│  interface/gemm.c                                           │
│    └── get_gemm_optimal_nthreads() → 计算最优线程数           │
│    └── gemm_thread_mn/m/n/variable → 任务分配策略选择        │
├─────────────────────────────────────────────────────────────┤
│  driver/level3/level3_thread.c                              │
│    └── gemm_driver() → 创建任务队列、分区                    │
│    └── inner_thread() → 线程工作函数、B矩阵共享              │
├─────────────────────────────────────────────────────────────┤
│  driver/others/blas_server.c                                │
│    └── blas_thread_init() → 初始化线程池                     │
│    ├── blas_thread_server() → 工作线程主循环                 │
│    ├── exec_blas() → 执行任务队列                            │
│    └── exec_blas_async/wait() → 异步执行/等待                │
├─────────────────────────────────────────────────────────────┤
│  driver/others/memory.c                                     │
│    └── blas_memory_alloc() → 内存池分配                      │
│    └── blas_memory_free() → 内存池释放                       │
│    └── get_memory_table() → TLS线程本地存储                  │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 关键源文件路径

| 文件              | 路径                              | 核心功能                |
| --------------- | ------------------------------- | ------------------- |
| gemm.c          | `interface/gemm.c`              | GEMM接口入口、线程数决策      |
| level3_thread.c | `driver/level3/level3_thread.c` | 任务分割、inner_thread实现 |
| blas_server.c   | `driver/others/blas_server.c`   | 线程池管理、任务调度          |
| memory.c        | `driver/others/memory.c`        | 内存池、TLS存储           |
| common_thread.h | `common_thread.h`               | 线程相关数据结构定义          |

---

## 二、线程池实现

### 2.1 核心数据结构

**位置**: `blas_server.c:134-146`

```c
typedef struct {
  blas_queue_t * volatile queue   __attribute__((aligned(128)));
#if defined(OS_LINUX) && !defined(NO_AFFINITY)
  int node;                         // NUMA节点编号
#endif
  volatile long status;             // 线程状态
  pthread_mutex_t lock;             // 互斥锁
  pthread_cond_t wakeup;            // 条件变量（唤醒）
} thread_status_t;
```

**全局变量**:

```c
static thread_status_t thread_status[MAX_CPU_NUMBER];  // 线程状态数组
static pthread_t blas_threads[MAX_CPU_NUMBER];         // 线程ID数组
int blas_server_avail = 0;                             // 线程池是否可用
int blas_num_threads = 0;                              // 线程池大小
```

### 2.2 线程状态常量

| 状态值 | 宏定义                    | 含义            |
| --- | ---------------------- | ------------- |
| 2   | `THREAD_STATUS_SLEEP`  | 线程休眠，等待条件变量唤醒 |
| 4   | `THREAD_STATUS_WAKEUP` | 线程唤醒，准备执行任务   |

### 2.3 线程池初始化

**位置**: `blas_server.c:549-624`

```c
int blas_thread_init(void) {
  if (blas_server_avail) return 0;  // 避免重复初始化

  LOCK_COMMAND(&server_lock);
  adjust_thread_buffers();

  if (!blas_server_avail) {
    // 设置超时参数
    thread_timeout_env = openblas_thread_timeout();
    if (thread_timeout_env > 0) {
      if (thread_timeout_env < 4) thread_timeout_env = 4;
      if (thread_timeout_env > 30) thread_timeout_env = 30;
      thread_timeout = (1 << thread_timeout_env);
    }

    // 创建 blas_num_threads - 1 个工作线程
    for (i = 0; i < blas_num_threads - 1; i++) {
      atomic_store_queue(&thread_status[i].queue, (blas_queue_t *)0);
      thread_status[i].status = THREAD_STATUS_WAKEUP;

      pthread_mutex_init(&thread_status[i].lock, NULL);
      pthread_cond_init(&thread_status[i].wakeup, NULL);

      pthread_create(&blas_threads[i], NULL, &blas_thread_server, (void *)i);
    }

    blas_server_avail = 1;
  }

  UNLOCK_COMMAND(&server_lock);
  return 0;
}
```

**设计要点**:

- **主线程参与计算**: 只创建 N-1 个工作线程，主线程作为第N个线程直接执行第一个任务
- **惰性初始化**: 首次调用时才创建线程池
- **避免重复**: 通过 `blas_server_avail` 标志防止多次初始化

### 2.4 工作线程主循环

**位置**: `blas_server.c:379-489`

```c
static void* blas_thread_server(void *arg) {
  BLASLONG cpu = (BLASLONG)arg;
  unsigned int last_tick;
  blas_queue_t *queue, *tscq;

  // 设置CPU亲和性 (Linux + NO_AFFINITY未定义)
#if defined(OS_LINUX) && !defined(NO_AFFINITY)
  if (!increased_threads)
    thread_status[cpu].node = gotoblas_set_affinity(cpu + 1);
  else
    thread_status[cpu].node = gotoblas_set_affinity(-1);
#endif

  while (1) {
    last_tick = (unsigned int)rpcc();
    tscq = atomic_load_queue(&thread_status[cpu].queue);

    // === 第一阶段：自旋等待（短时间任务） ===
    while (!tscq || tscq == 0x1) {
      YIELDING;  // CPU pause/yield指令

      // 超时检查
      if ((unsigned int)rpcc() - last_tick > thread_timeout) {
        // === 第二阶段：休眠等待（长时间无任务） ===
        if (!atomic_load_queue(&thread_status[cpu].queue)) {
          pthread_mutex_lock(&thread_status[cpu].lock);
          thread_status[cpu].status = THREAD_STATUS_SLEEP;

          // 条件变量等待
          while (thread_status[cpu].status == THREAD_STATUS_SLEEP &&
                 !atomic_load_queue(&thread_status[cpu].queue)) {
            pthread_cond_wait(&thread_status[cpu].wakeup, 
                              &thread_status[cpu].lock);
          }
          pthread_mutex_unlock(&thread_status[cpu].lock);
        }
        last_tick = (unsigned int)rpcc();
      }

      tscq = atomic_load_queue(&thread_status[cpu].queue);
    }

    // === 第三阶段：执行任务 ===
    queue = atomic_load_queue(&thread_status[cpu].queue);
    if ((long)queue == -1) break;  // 收到退出信号

    if (queue) {
      exec_threads(cpu, queue, 0);
    }
  }

  return NULL;
}
```

**核心机制**:

| 阶段   | 策略                  | 目的              |
| ---- | ------------------- | --------------- |
| 自旋等待 | `YIELDING` 指令循环     | 短任务快速响应，避免上下文切换 |
| 休眠等待 | `pthread_cond_wait` | 长时间空闲时节省CPU资源   |
| 任务执行 | `exec_threads()`    | 调用实际计算函数        |

**超时参数**: 默认 `THREAD_TIMEOUT=28`，即超时时间 = 2^28 个时钟周期

### 2.5 任务分发流程

**位置**: `blas_server.c:636-755`

```c
int exec_blas_async(BLASLONG pos, blas_queue_t *queue) {
  BLASLONG i = 0;
  blas_queue_t *current = queue;

#if defined(OS_LINUX) && !defined(NO_AFFINITY)
  int node = get_node();
  int nodes = get_num_nodes();
#endif

  blas_lock(&exec_queue_lock);

  while (queue) {
    queue->position = pos;

#if defined(OS_LINUX) && !defined(NO_AFFINITY)
    // NUMA节点映射模式
    if (queue->mode & BLAS_NODE) {
      do {
        while ((thread_status[i].node != node || 
                atomic_load_queue(&thread_status[i].queue)) &&
               (i < blas_num_threads - 1)) i++;
        if (i < blas_num_threads - 1) break;
        i++;
        if (i >= blas_num_threads - 1) {
          i = 0;
          node++;
          if (node >= nodes) node = 0;
        }
      } while (1);
    } else {
      // 查找空闲线程
      tsiq = atomic_load_queue(&thread_status[i].queue);
      while (tsiq) {
        i++;
        if (i >= blas_num_threads - 1) i = 0;
        tsiq = atomic_load_queue(&thread_status[i].queue);
      }
    }
#else
    // 查找空闲线程（无NUMA支持）
    tsiq = atomic_load_queue(&thread_status[i].queue);
    while (tsiq) {
      i++;
      if (i >= blas_num_threads - 1) i = 0;
      tsiq = atomic_load_queue(&thread_status[i].queue);
    }
#endif

    // 分配任务到线程
    queue->assigned = i;
    MB;
    atomic_store_queue(&thread_status[i].queue, queue);

    queue = queue->next;
    pos++;
  }

  blas_unlock(&exec_queue_lock);

  // 唤醒休眠线程
  while (current) {
    pos = current->assigned;
    tspq = atomic_load_queue(&thread_status[pos].queue);

    if ((BLASULONG)tspq > 1) {
      pthread_mutex_lock(&thread_status[pos].lock);
      if (thread_status[pos].status == THREAD_STATUS_SLEEP) {
        thread_status[pos].status = THREAD_STATUS_WAKEUP;
        pthread_cond_signal(&thread_status[pos].wakeup);
      }
      pthread_mutex_unlock(&thread_status[pos].lock);
    }
    current = current->next;
  }

  return 0;
}
```

### 2.6 同步等待完成

**位置**: `blas_server.c:757-781`

```c
int exec_blas_async_wait(BLASLONG num, blas_queue_t *queue) {
  blas_queue_t *tsqq;

  while ((num > 0) && queue) {
    tsqq = atomic_load_queue(&thread_status[queue->assigned].queue);

    // 自旋等待任务完成
    while (tsqq) {
      YIELDING;
      tsqq = atomic_load_queue(&thread_status[queue->assigned].queue);
    }

    queue = queue->next;
    num--;
  }

  MB;  // 内存屏障确保结果可见
  return 0;
}
```

---

## 三、内存池实现

### 3.1 核心数据结构

**位置**: `memory.c:530-559`

```c
struct alloc_t {
  int used;                              // 是否正在使用
  int attr;                              // 释放时的特殊属性
  void (*release_func)(struct alloc_t *);// 释放函数指针
  char pad[64 - 2 * sizeof(int) - sizeof(void(*)]; // 64字节对齐填充
};

static const int allocation_block_size = BUFFER_SIZE + sizeof(struct alloc_t);
```

**BUFFER_SIZE**: 由编译参数决定，需满足:

```
BUFFER_SIZE >= SGEMM_DEFAULT_P * SGEMM_DEFAULT_Q * 4 * 2
BUFFER_SIZE >= DGEMM_DEFAULT_P * DGEMM_DEFAULT_Q * 8 * 2
BUFFER_SIZE >= CGEMM_DEFAULT_P * CGEMM_DEFAULT_Q * 8 * 2
BUFFER_SIZE >= ZGEMM_DEFAULT_P * ZGEMM_DEFAULT_Q * 16 * 2
```

### 3.2 线程本地存储 (TLS)

**位置**: `memory.c:561-634`

```c
#if defined(SMP)
#  if defined(OS_WINDOWS)
static DWORD local_storage_key = 0;
#  else
static pthread_key_t local_storage_key = 0;
#  endif
#endif

static __inline struct alloc_t ** get_memory_table(void) {
#if defined(SMP)
  LOCK_COMMAND(&key_lock);
  lsk = local_storage_key;
  UNLOCK_COMMAND(&key_lock);

  if (!lsk) {
    blas_memory_init();  // 惰性初始化
  }

#  if defined(OS_WINDOWS)
  struct alloc_t **local_memory_table = (struct alloc_t **)TlsGetValue(local_storage_key);
#  else
  struct alloc_t **local_memory_table = (struct alloc_t **)pthread_getspecific(local_storage_key);
#  endif
#else
  static struct alloc_t **local_memory_table = NULL;
#endif

  if (!local_memory_table) {
    local_memory_table = (struct alloc_t **)malloc(sizeof(struct alloc_t *) * NUM_BUFFERS);
    memset(local_memory_table, 0, sizeof(struct alloc_t *) * NUM_BUFFERS);

#if defined(SMP)
#  if defined(OS_WINDOWS)
    TlsSetValue(local_storage_key, (void*)local_memory_table);
#  else
    pthread_setspecific(local_storage_key, (void*)local_memory_table);
#  endif
#endif
  }

  return local_memory_table;
}
```

**TLS优势**:

- 每个线程独立的内存分配表
- 避免全局锁竞争
- 支持NUMA亲和性

### 3.3 内存分配流程

**位置**: `memory.c:1243-1330`

```c
void *blas_memory_alloc(int unused) {
  struct alloc_t **alloc_table = get_memory_table();

  // === Step 1: 查找空闲槽位 ===
  position = 0;
  do {
    if (!alloc_table[position] || !alloc_table[position]->used)
      goto allocation;
    position++;
  } while (position < NUM_BUFFERS);

  goto error;  // 超出最大缓冲区数量

allocation:

  // === Step 2: 检查槽位是否已分配 ===
  alloc_info = alloc_table[position];
  if (!alloc_info) {
    do {
      map_address = (void *)-1;

      // === Step 3: 尝试多种分配策略 ===
      func = &memoryalloc[0];
      while ((*func != NULL) && (map_address == (void *)-1)) {
        map_address = (*func)((void *)base_address);

        // HugeTLB成功标记
        if ((*func == alloc_hugetlb) && (map_address != (void *)-1))
          hugetlb_allocated = 1;

        func++;
      }

      if (((BLASLONG)map_address) == -1)
        base_address = 0UL;
      if (base_address)
        base_address += allocation_block_size + FIXED_PAGESIZE;

    } while ((BLASLONG)map_address == -1);

    alloc_table[position] = alloc_info = map_address;
  }

  // === Step 4: 标记为已使用 ===
  alloc_info->used = 1;

  // 返回实际数据区域（跳过alloc_t头）
  return (void *)(((char *)alloc_info) + sizeof(struct alloc_t));

error:
  printf("OpenBLAS: Program will terminate because you tried to allocate "
         "too many TLS memory regions.\n");
  printf("This library was built to support a maximum of %d threads\n", NUM_BUFFERS);
  return NULL;
}
```

### 3.4 内存释放

**位置**: `memory.c:1332-1361`

```c
void blas_memory_free(void *buffer) {
  // 获取实际分配结构（回退指针）
  struct alloc_t *alloc_info = (void *)(((char *)buffer) - sizeof(struct alloc_t));

  // 仅标记为未使用（不真正释放）
  alloc_info->used = 0;

  return;
}
```

**设计要点**:

- **不真正释放**: 只标记 `used=0`，内存保留供后续使用
- **避免反复分配**: 特别适合频繁调用BLAS的场景
- **释放时机**: 仅在 `blas_shutdown()` 或进程退出时真正释放

### 3.5 分配策略优先级

| 优先级 | 策略      | 适用条件                | 优势            |
| --- | ------- | ------------------- | ------------- |
| 1   | mmap    | Linux/Unix          | 大页支持、NUMA亲和   |
| 2   | malloc  | Windows/备用          | 通用性强          |
| 3   | HugeTLB | Linux + SHM_HUGETLB | 大矩阵减少TLB miss |
| 4   | shm     | SYSV IPC            | 特定系统需求        |

---

## 四、GEMM多线程任务分配策略

### 4.1 任务队列结构

**位置**: `common_thread.h:100-134`

```c
typedef struct blas_queue {
  void *routine;              // 执行函数指针
  BLASLONG position;          // 位置索引
  BLASLONG assigned;          // 分配的线程ID

  blas_arg_t *args;           // BLAS参数结构
  void *range_m;              // M维度范围 [from, to]
  void *range_n;              // N维度范围 [from, to]
  void *sa, *sb;              // 工作缓冲区指针

  struct blas_queue *next;    // 链表下一节点

#if defined(__WIN32__) || defined(__CYGWIN32__)
  CRITICAL_SECTION lock;
  HANDLE finish;
  volatile int finished;
#else
  pthread_mutex_t lock;
  pthread_cond_t finished;
#endif

  int mode, status;           // 执行模式、状态
} blas_queue_t;
```

### 4.2 四种任务分配方式

| 函数                     | 源文件                      | 分割维度     | 适用场景    |
| ---------------------- | ------------------------ | -------- | ------- |
| `gemm_thread_m`        | `gemm_thread_m.c`        | 仅沿M分割    | M较大、N较小 |
| `gemm_thread_n`        | `gemm_thread_n.c`        | 仅沿N分割    | N较大、M较小 |
| `gemm_thread_mn`       | `gemm_thread_mn.c`       | M×N 2D分割 | M和N都较大  |
| `gemm_thread_variable` | `gemm_thread_variable.c` | 自适应分割    | 动态优化    |

### 4.3 分割规则表

**位置**: `gemm_thread_mn.c:43-61`

```c
static const int divide_rule[][2] = {
  { 0,  0},                     // 0线程（无效）
  { 1,  1}, { 1,  2}, { 1,  3}, { 2,  2},  // 1-4线程
  { 1,  5}, { 2,  3}, { 1,  7}, { 2,  4},  // 5-8线程
  { 3,  3}, { 2,  5}, { 1, 11}, { 2,  6},  // 9-12线程
  { 1, 13}, { 2,  7}, { 3,  5}, { 4,  4},  // 13-16线程
  { 1, 17}, { 3,  6}, { 1, 19}, { 4,  5},  // 17-20线程
  // ... 最大支持64线程
  { 8,  8},                     // 64线程: 8×8分割
};
```

**含义**: `divide_rule[n][0]` = M方向线程数, `divide_rule[n][1]` = N方向线程数

### 4.4 2D分割算法详解

**位置**: `level3_thread.c:822-891`

```c
int CNAME(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, 
          IFLOAT *sa, IFLOAT *sb, BLASLONG mypos) {
  BLASLONG m = args->m;
  BLASLONG n = args->n;
  BLASLONG nthreads_m, nthreads_n;
  int switch_ratio = gotoblas->switch_ratio;  // 或 SWITCH_RATIO

  // === Step 1: 确定M方向线程数 ===
  if (m < 2 * switch_ratio) {
    nthreads_m = 1;  // M太小，只沿N分割
  } else {
    nthreads_m = args->nthreads;
    // 逐步减半直到满足最小宽度要求
    while (m < nthreads_m * switch_ratio) {
      nthreads_m = nthreads_m / 2;
    }
  }

  // === Step 2: 确定N方向线程数 ===
  if (n < switch_ratio * nthreads_m) {
    nthreads_n = 1;
  } else {
    nthreads_n = (n + switch_ratio * nthreads_m - 1) / (switch_ratio * nthreads_m);

    // 确保不超过总线程数
    if (nthreads_m * nthreads_n > args->nthreads) {
      nthreads_n = blas_quickdivide(args->nthreads, nthreads_m);
    }

    // === Step 3: 优化分割比例 ===
    // 目标: 使每个线程处理的子矩阵接近正方形
    // 优化函数: n * nthreads_m + m * nthreads_n 最小化
    BLASLONG cost = 0, div = 0;
    for (i = 1; i <= sqrt(nthreads_m); i++) {
      if (nthreads_m % i) continue;
      BLASLONG j = nthreads_m / i;
      BLASLONG cost_i = n * j + m * nthreads_n * i;
      BLASLONG cost_j = n * i + m * nthreads_n * j;
      if (cost == 0 || cost_i < cost) { cost = cost_i; div = i; }
      if (cost_j < cost) { cost = cost_j; div = j; }
    }

    if (div > 1) {
      nthreads_m /= div;
      nthreads_n *= div;
    }
  }

  // === Step 4: 执行计算 ===
  if (nthreads_m * nthreads_n <= 1) {
    GEMM_LOCAL(args, range_m, range_n, sa, sb, 0);  // 单线程
  } else {
    args->nthreads = nthreads_m * nthreads_n;
    gemm_driver(args, range_m, range_n, sa, sb, nthreads_m, nthreads_n);
  }

  return 0;
}
```

### 4.5 B矩阵共享机制（核心创新）

**位置**: `level3_thread.c:240-555`

这是OpenBLAS多线程GEMM最核心的优化——**B矩阵广播共享**。

#### 4.5.1 同步数据结构

```c
typedef struct {
  volatile BLASLONG working[MAX_CPU_NUMBER][CACHE_LINE_SIZE * DIVIDE_RATE_MAX];
} job_t;

// working数组用于线程间同步
// job[i].working[j][k] = 线程j告知线程i: B的k分片已就绪（存储指针）
```

#### 4.5.2 inner_thread 完整流程

```c
static int inner_thread(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n,
                        IFLOAT *sa, IFLOAT *sb, BLASLONG mypos) {
  job_t *job = (job_t *)args->common;
  BLASLONG nthreads_m = args->nthreads;
  if (range_m) nthreads_m = range_m[-1];

  // 计算线程在2D网格中的位置
  mypos_n = blas_quickdivide(mypos, nthreads_m);  // mypos_n = mypos / nthreads_m
  mypos_m = mypos - mypos_n * nthreads_m;         // mypos_m = mypos % nthreads_m

  // 获取M和N的范围
  m_from = range_m[mypos_m + 0];
  m_to   = range_m[mypos_m + 1];
  n_from = range_n[mypos_n * nthreads_m];
  n_to   = range_n[(mypos_n + 1) * nthreads_m];

  // 初始化B缓冲区（DIVIDE_RATE分片）
  div_n = (n_to - n_from + divide_rate - 1) / divide_rate;
  buffer[0] = sb;
  for (i = 1; i < divide_rate; i++) {
    buffer[i] = buffer[i-1] + GEMM_Q * ((div_n + GEMM_UNROLL_N - 1) / GEMM_UNROLL_N)
                * GEMM_UNROLL_N * COMPSIZE;
  }

  // === 主循环: 遍历K维度 ===
  for (ls = 0; ls < k; ls += min_l) {
    min_l = GEMM_Q;  // 或动态调整

    // === Step A: 复制A子块到私有缓冲区 ===
    min_i = m_to - m_from;  // 动态调整
    ICOPY_OPERATION(min_l, min_i, a, lda, ls, m_from, sa);

    // === Step B: 复制B子块并设置同步标志 ===
    for (js = n_from, bufferside = 0; js < n_to; js += div_n, bufferside++) {

      // 等待其他线程释放缓冲区
      for (i = 0; i < args->nthreads; i++)
        while (job[mypos].working[i][CACHE_LINE_SIZE * bufferside]) { YIELDING; }
      MB;

      // 复制B子块
      for (jjs = js; jjs < MIN(n_to, js + div_n); jjs += min_jj) {
        OCOPY_OPERATION(min_l, min_jj, b, ldb, ls, jjs,
                        buffer[bufferside] + pad_min_l * (jjs - js) * COMPSIZE);

        // 执行本地内核（使用自己的A和自己的B）
        KERNEL_OPERATION(min_i, min_jj, min_l, alpha, sa,
                         buffer[bufferside] + pad_min_l * (jjs - js) * COMPSIZE,
                         c, ldc, m_from, jjs);
      }

      WMB;

      // === 关键: 设置同步标志，广播B给同行线程 ===
      for (i = mypos_n * nthreads_m; i < (mypos_n + 1) * nthreads_m; i++)
        job[mypos].working[i][CACHE_LINE_SIZE * bufferside] = (BLASLONG)buffer[bufferside];
    }

    // === Step C: 使用其他线程的B ===
    current = mypos;
    do {
      current++;
      if (current >= (mypos_n + 1) * nthreads_m)
        current = mypos_n * nthreads_m;

      // 遍历B的所有分片
      for (js = range_n[current], bufferside = 0; 
           js < range_n[current + 1]; 
           js += div_n, bufferside++) {

        if (current != mypos) {
          // === 等待其他线程的B就绪 ===
          while (job[current].working[mypos][CACHE_LINE_SIZE * bufferside] == 0)
            { YIELDING; }
          MB;

          // === 使用其他线程复制的B + 自己的A ===
          KERNEL_OPERATION(min_i, MIN(range_n[current + 1] - js, div_n), min_l,
                           alpha, sa,
                           (IFLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
                           c, ldc, m_from, js);
        }

        // === 清除同步标志 ===
        if (m_to - m_from == min_i) {
          WMB;
          job[current].working[mypos][CACHE_LINE_SIZE * bufferside] &= 0;
        }
      }
    } while (current != mypos);

    // === Step D: 继续处理A的后续分片 ===
    for (is = m_from + min_i; is < m_to; is += min_i) {
      ICOPY_OPERATION(min_l, min_i, a, lda, ls, is, sa);

      // 使用已就绪的B（无需等待）
      current = mypos;
      do {
        // ... 类似流程
      } while (current != mypos);
    }
  }

  // === Step E: 等待所有线程完成对本线程B的使用 ===
  for (i = 0; i < args->nthreads; i++) {
    for (js = 0; js < divide_rate; js++) {
      while (job[mypos].working[i][CACHE_LINE_SIZE * js]) { YIELDING; }
    }
  }
  MB;

  return 0;
}
```

#### 4.5.3 共享机制示意图

```
              N维度分割
    ┌─────────────────────────────────────────┐
    │   N[0]    │   N[1]    │   N[2]    │ N[3]│
    ├───────────┼───────────┼───────────┼─────┤
M[0]│ Thread0   │ Thread1   │ Thread2   │ T3  │
    │ A[0]私有  │ A[0]私有  │ A[0]私有  │ ... │
    │ B[0]广播─→│ B[0]广播─→│ B[0]广播─→│     │
    │           │           │           │     │
M[1]│ Thread4   │ Thread5   │ Thread6   │ T7  │
    │ A[1]私有  │ A[1]私有  │ A[1]私有  │ ... │
    │ B[1]广播─→│ B[1]广播─→│ B[1]广播─→│     │
    └───────────┴───────────┴───────────┴─────┘

    每个线程:
    1. 复制自己的A子块（私有）
    2. 复制自己的B子块（共享给同行线程）
    3. 设置 job[mypos].working[other_thread] = buffer指针
    4. 使用所有同行线程的B（包括自己的）
    5. 清除同步标志
```

**优势**:

- B矩阵只复制一次，多个M方向线程共享使用
- 减少内存复制开销
- 提高缓存利用率（B数据被多次使用）

---

## 五、ARM架构特殊处理

### 5.1 Neoverse V1/V2 线程数优化

**位置**: `interface/gemm.c:193-252`

```c
#if defined(NEOVERSEV1)
static inline int get_gemm_optimal_nthreads_neoversev1(double MNK, int ncpu) {
  return
      MNK < 262144L      ? 1          // < 512×512×1
    : MNK < 1124864L     ? MIN(ncpu, 6)
    : MNK < 7880599L     ? MIN(ncpu, 12)
    : MNK < 17173512L    ? MIN(ncpu, 16)
    : MNK < 33386248L    ? MIN(ncpu, 20)
    : MNK < 57066625L    ? MIN(ncpu, 24)
    : MNK < 91733851L    ? MIN(ncpu, 32)
    : MNK < 265847707L   ? MIN(ncpu, 40)
    : MNK < 458314011L   ? MIN(ncpu, 48)
    : MNK < 729000000L   ? MIN(ncpu, 56)
    : ncpu;                            // 大矩阵使用全部核心
}
#endif

#if defined(NEOVERSEV2)
static inline int get_gemm_optimal_nthreads_neoversev2(double MNK, int ncpu) {
  return
      MNK < 125000L      ? 1
    : MNK < 1092727L     ? MIN(ncpu, 6)
    : MNK < 2628072L     ? MIN(ncpu, 8)
    : MNK < 8000000L     ? MIN(ncpu, 12)
    : MNK < 20346417L    ? MIN(ncpu, 16)
    : MNK < 57066625L    ? MIN(ncpu, 24)
    : MNK < 91125000L    ? MIN(ncpu, 28)
    : MNK < 238328000L   ? MIN(ncpu, 40)
    : MNK < 454756609L   ? MIN(ncpu, 48)
    : MNK < 857375000L   ? MIN(ncpu, 56)
    : MNK < 1073741824L  ? MIN(ncpu, 64)
    : ncpu;
}
#endif

// 动态架构时根据CPU名称选择
#if defined(DYNAMIC_ARCH)
  if (strcmp(gotoblas_corename(), "neoversev1") == 0) {
    return get_gemm_optimal_nthreads_neoversev1(MNK, ncpu);
  }
  if (strcmp(gotoblas_corename(), "neoversev2") == 0) {
    return get_gemm_optimal_nthreads_neoversev2(MNK, ncpu);
  }
#endif
```

**设计思路**:

- Neoverse V1/V2 是ARM服务器级CPU（如AWS Graviton3/4）
- 基于经验数据优化不同矩阵规模的最优线程数
- 避免小矩阵多线程带来的额外同步开销

### 5.2 ARM64 GEMM内核配置

**位置**: `kernel/arm64/`

| KERNEL文件              | 适用CPU             | 特性                    |
| --------------------- | ----------------- | --------------------- |
| `KERNEL.ARMV8`        | 基础ARMv8           | NEON优化                |
| `KERNEL.ARMV8SVE`     | SVE支持             | SVE可伸缩向量              |
| `KERNEL.NEOVERSEV1`   | Neoverse V1       | 继承ARMV8SVE + BFLOAT16 |
| `KERNEL.NEOVERSEV2`   | Neoverse V2       | SVE2优化                |
| `KERNEL.NEOVERSEN1`   | Neoverse N1       | 高性能NEON               |
| `KERNEL.NEOVERSEN2`   | Neoverse N2       | SVE + BFLOAT16        |
| `KERNEL.A64FX`        | Fujitsu A64FX     | 512-bit SVE           |
| `KERNEL.CORTEXA53`    | Cortex-A53        | 低功耗优化                 |
| `KERNEL.THUNDERX2T99` | Marvell ThunderX2 | 高吞吐                   |

### 5.3 Neoverse V1配置示例

**位置**: `kernel/arm64/KERNEL.NEOVERSEV1`

```makefile
include $(KERNELDIR)/KERNEL.ARMV8SVE

# GEMV内核
SGEMVNKERNEL = gemv_n_sve_v1x3.c
DGEMVNKERNEL = gemv_n_sve_v1x3.c
SGEMVTKERNEL = gemv_t_sve_v1x3.c
DGEMVTKERNEL = gemv_t_sve_v1x3.c

# AXPY内核
SAXPYKERNEL = axpy_sve.c
DAXPYKERNEL = axpy_sve.c

# BFLOAT16支持
ifeq ($(BUILD_BFLOAT16), 1)
BGEMM_BETA    = bgemm_beta_neon.c
BGEMMKERNEL   = bgemm_kernel_2vlx4_neoversev1.c
BGEMMONCOPY   = bgemm_ncopy_4_neoversev1.c
BGEMMOTCOPY   = bgemm_tcopy_4_neoversev1.c
SBGEMM_BETA   = sbgemm_beta_neoversev1.c
SBGEMMKERNEL  = bgemm_kernel_2vlx4_neoversev1.c
endif

# GER内核
SGERKERNEL = ger_sve_v1x3.c
DGERKERNEL = ger_sve_v1x3.c
```

### 5.4 CPU亲和性设置

**位置**: `blas_server.c:338-397`

```c
#if defined(OS_LINUX) && !defined(NO_AFFINITY)
int gotoblas_set_affinity(int cpu_id);
int get_node(void);  // 获取NUMA节点
#endif

static void* blas_thread_server(void *arg) {
  BLASLONG cpu = (BLASLONG)arg;

#if defined(OS_LINUX) && !defined(NO_AFFINITY)
  if (!increased_threads)
    thread_status[cpu].node = gotoblas_set_affinity(cpu + 1);
  else
    thread_status[cpu].node = gotoblas_set_affinity(-1);
#endif

  // ...
}
```

**NUMA任务分发**:

```c
if (queue->mode & BLAS_NODE) {
  // 在同一NUMA节点内寻找线程
  do {
    while ((thread_status[i].node != node || 
            atomic_load_queue(&thread_status[i].queue)) &&
           (i < blas_num_threads - 1)) i++;

    if (i < blas_num_threads - 1) break;

    i++;
    if (i >= blas_num_threads - 1) {
      i = 0;
      node++;
      if (node >= nodes) node = 0;
    }
  } while (1);
}
```

---

## 六、同步机制

### 6.1 内存屏障定义

**位置**: `common.h`

```c
// ARM64专用屏障
#ifdef __aarch64__
#define MB   __asm__ __volatile__ ("dmb sy" ::: "memory")   // 全屏障
#define WMB  __asm__ __volatile__ ("dmb st" ::: "memory")   // 写屏障
#define RMB  __asm__ __volatile__ ("dmb ld" ::: "memory")   // 读屏障
#define YIELDING __asm__ __volatile__ ("yield" ::: "memory") // ARM yield指令
#endif

// x86屏障
#ifdef __x86_64__
#define MB   __asm__ __volatile__ ("mfence" ::: "memory")
#define WMB  __asm__ __volatile__ ("sfence" ::: "memory")
#define RMB  __asm__ __volatile__ ("lfence" ::: "memory")
#define YIELDING __asm__ __volatile__ ("pause" ::: "memory")
#endif
```

### 6.2 ARM64特殊处理

**位置**: `blas_server.c:851-852`

```c
// arm: make sure results from other threads are visible
MB;  // 全内存屏障
```

**位置**: `blas_server.c:661-667`

```c
#ifdef CONSISTENT_FPCSR
#ifdef __aarch64__
  __asm__ __volatile__ ("mrs %0, fpcr" : "=r" (queue->sse_mode));
#else
  __asm__ __volatile__ ("fnstcw %0"  : "=m" (queue->x87_mode));
  __asm__ __volatile__ ("stmxcsr %0" : "=m" (queue->sse_mode));
#endif
#endif
```

### 6.3 原子队列操作

**位置**: `blas_server.c:148-154`

```c
#ifdef HAVE_C11
#define atomic_load_queue(p)    __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define atomic_store_queue(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#else
// GCC fallback
#define atomic_load_queue(p)    (blas_queue_t*)(*(volatile blas_queue_t**)(p))
#define atomic_store_queue(p, v) (*(volatile blas_queue_t* volatile*)(p) = (v))
#endif
```

**内存序语义**:

- `ACQUIRE`: 保证后续读取不会重排到此操作之前
- `RELEASE`: 保证之前的写入不会重排到此操作之后

---

## 七、核心设计思想总结

| 设计点        | 实现方式                       | 目的           |
| ---------- | -------------------------- | ------------ |
| **线程池模型**  | 预创建N-1工作线程，主线程参与计算         | 避免线程创建开销     |
| **自旋+休眠**  | 短时间YIELDING自旋，超时后条件变量休眠    | 平衡响应速度与CPU消耗 |
| **TLS内存池** | 每线程独立内存表(pthread_key)，惰性分配 | 避免锁竞争，NUMA亲和 |
| **惰性释放**   | blas_memory_free只标记used=0  | 减少反复分配开销     |
| **B矩阵共享**  | job[].working[][]同步数组广播B指针 | 减少复制，提高缓存利用  |
| **2D任务分割** | 按M×N分割，优化子矩阵形状             | 负载均衡，减少边界开销  |
| **ARM优化**  | 基于MNK动态线程数表，NUMA亲和         | 适配ARM服务器特性   |
| **无锁队列**   | 原子操作 + Acquire/Release内存序  | 减少同步开销       |

---

## 八、调用流程完整示意

```
┌────────────────────────────────────────────────────────────────┐
│                       用户调用 gemm()                           │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ interface/gemm.c: NAME()                                       │
│   ├── 参数校验（M, N, K, lda, ldb, ldc）                        │
│   ├── MNK = (double)M * N * K                                  │
│   ├── nthreads = get_gemm_optimal_nthreads(MNK)                │
│   │   └── NeoverseV1/V2: 基于MNK表的动态线程数                   │
│   │   └── Generic: MNK/(SMP_THRESHOLD_MIN*GEMM_MULTITHREAD_THRESHOLD)│
│   └── args.nthreads = nthreads                                 │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ gemm_thread_mn(mode, &args, NULL, NULL, gemm[...], sa, sb, nt) │
│   ├── divM = divide_rule[nthreads][0]                          │
│   ├── divN = divide_rule[nthreads][1]                          │
│   ├── 按divM分割M维度: range_M[0..divM]                         │
│   ├── 按divN分割N维度: range_N[0..divN]                         │
│   ├── 构建 blas_queue_t 队列:                                  │
│   │   └── queue[i].routine = gemm_driver                       │
│   │   └── queue[i].range_m = &range_M[i]                       │
│   │   └── queue[i].range_n = &range_N[j]                       │
│   └── exec_blas(procs, queue)                                  │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ blas_server.c: exec_blas(num, queue)                           │
│   ├── if (num > 1 && queue->next)                              │
│   │   └── exec_blas_async(1, queue->next)  // 分发到工作线程    │
│   ├── 主线程执行 queue[0]:                                      │
│   │   └── gemm_driver(args, range_m, range_n, sa, sb, ...)     │
│   ├── if (num > 1)                                             │
│   │   └── exec_blas_async_wait(num-1, queue->next)             │
│   └── MB;  // ARM64内存屏障                                    │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ blas_server.c: exec_blas_async(pos, queue)                     │
│   ├── blas_lock(&exec_queue_lock)                              │
│   ├── 遍历队列:                                                 │
│   │   ├── 查找空闲线程 i                                        │
│   │   ├── queue->assigned = i                                  │
│   │   └── atomic_store_queue(&thread_status[i].queue, queue)   │
│   ├── blas_unlock(&exec_queue_lock)                            │
│   └── 唤醒休眠线程: pthread_cond_signal(&thread_status[i].wakeup)│
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ blas_thread_server(cpu) [工作线程]                              │
│   ├── atomic_load_queue(&thread_status[cpu].queue)             │
│   ├── 自旋等待: YIELDING                                        │
│   ├── 或休眠: pthread_cond_wait(&wakeup, &lock)                │
│   └── exec_threads(cpu, queue, 0)                              │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ level3_thread.c: gemm_driver(args, range_m, range_n, ...)      │
│   ├── 初始化 job_t 同步结构                                     │
│   ├── 设置 range_M[-1] = nthreads_m                            │
│   ├── 设置 range_N[-1] = nthreads_n                            │
│   ├── 按GEMM_P/GEMM_Q分割维度                                   │
│   ├── 清除同步标志: job[i].working[j][k] = 0                    │
│   ├── 构建 blas_queue_t 队列:                                  │
│   │   └── queue[i].routine = inner_thread                      │
│   │   └── queue[i].args = &newarg                              │
│   │   └── queue[i].common = (void *)job                        │
│   └── exec_blas(nthreads, queue)                               │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ level3_thread.c: inner_thread(args, range_m, range_n, mypos)   │
│   ├── 计算线程位置:                                             │
│   │   └── mypos_n = mypos / nthreads_m                         │
│   │   └── mypos_m = mypos % nthreads_m                         │
│   ├── 获取范围:                                                 │
│   │   └── m_from/m_to, n_from/n_to                             │
│   ├── 遍历K维度:                                                │
│   │   ├── ICOPY_OPERATION(min_l, min_i, A, sa)  // 复制A私有    │
│   │   ├── OCOPY_OPERATION(min_l, min_jj, B, buffer) //复制B共享 │
│   │   ├── 设置同步标志: job[mypos].working[i][k] = buffer       │
│   │   ├── KERNEL_OPERATION(A, B, C)  // 本地计算               │
│   │   ├── 等待其他线程B就绪                                     │
│   │   ├── 使用其他线程B计算                                     │
│   │   ├── 清除同步标志                                          │
│   ├── 等待所有线程完成使用本线程的B                              │
│   └── MB;                                                      │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────┐
│ kernel/arm64/dgemm_kernel_sve_v1x8.S [实际计算内核]            │
│   ├── SVE向量加载: ld1d z0.d, p0/z, [x0]                       │
│   ├── FMLA乘加: fmla z4.d, p0/m, z0.d, z1.d                    │
│   ├── SVE向量存储: st1d z4.d, p0, [x4]                         │
│   └────────────────────────────────────────────────────────────┘
```

---

## 附录：关键代码位置索引

| 功能模块       | 文件                               | 关键行号      |
| ---------- | -------------------------------- | --------- |
| 线程状态结构     | `blas_server.c`                  | 134-146   |
| 线程池初始化     | `blas_server.c`                  | 549-624   |
| 工作线程循环     | `blas_server.c`                  | 379-489   |
| 任务分发       | `blas_server.c`                  | 636-755   |
| 内存分配结构     | `memory.c`                       | 530-559   |
| TLS存储      | `memory.c`                       | 561-634   |
| 内存分配       | `memory.c`                       | 1243-1330 |
| 任务队列结构     | `common_thread.h`                | 100-134   |
| 分割规则表      | `gemm_thread_mn.c`               | 43-61     |
| B矩阵共享      | `level3_thread.c`                | 240-555   |
| 2D分割算法     | `level3_thread.c`                | 822-891   |
| ARM线程数优化   | `interface/gemm.c`               | 193-252   |
| ARM屏障定义    | `common.h`                       | (条件编译块)   |
| Neoverse配置 | `kernel/arm64/KERNEL.NEOVERSEV1` | 全文件       |

---

## 参考资料

1. OpenBLAS源码: https://github.com/OpenMathLib/OpenBLAS
2. GotoBLAS2原始论文: "An Implementation of the BLAS Technical Forum Standard"
3. ARM SVE编程指南: https://developer.arm.com/architectures/instruction-sets/simd-isas/sve
4. NUMA亲和性最佳实践: https://www.kernel.org/doc/Documentation/admin-guide/mm/numa_memory_policy.rst

---

*文档生成于 OpenBLAS 源码深度分析过程*