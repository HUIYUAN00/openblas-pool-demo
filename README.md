# OpenBLAS风格线程池与内存池实现

## 目录结构（简洁紧凑）

```
demo/
├── pool.h              # 线程池与内存池头文件 (1.3K)
├── pool.c              # 线程池与内存池实现 (4.7K)
├── test.c              # 测试程序 (6.7K)
├── Makefile            # 编译配置
├── OpenBLAS_GEMM_Multithreading_Analysis.md  # OpenBLAS源码分析 (43K)
└── IMPLEMENTATION_DETAIL.md                  # 详细技术文档 (20K)
```

**总文件数: 6个**（源代码3个 + 编译1个 + 文档2个）

## 快速使用

```bash
make        # 编译
make run    # 运行测试
make clean  # 清理
```

## 测试结果

```
Test 1: Basic Thread Pool      ✓ PASSED
Test 2: Memory Pool            ✓ PASSED  
Test 3: Parallel Computation   ✓ PASSED
Test 4: Performance Benchmark  ✓ PASSED (16747 iter/s)

ALL TESTS PASSED SUCCESSFULLY
```

## API参考

### 线程池

```c
thread_pool_t pool;
thread_pool_init(&pool, 4);                  // 初始化4线程
thread_pool_parallel_for(&pool, func, arg, 4); // 并行执行4任务
thread_pool_shutdown(&pool);                 // 关闭
```

### 内存池

```c
memory_pool_t pool;
memory_pool_init(&pool, 64*1024, 8); // 64KB, 8缓冲区

void *buf = memory_alloc(&pool);     // 分配
memory_free(&pool, buf);             // 释放（惰性）

memory_pool_destroy(&pool);          // 销毁
```

## 核心设计（源自OpenBLAS）

| 特性       | 本实现         | OpenBLAS源码位置          |
|----------|------------|-------------------|
| 预创建线程    | N线程 + 条件变量  | blas_server.c:549 |
| 任务竞争领取   | 广播唤醒 + 计数  | blas_server.c:784 |
| 内存预分配    | malloc一次   | memory.c:595      |
| 惰性释放     | 标记未使用      | memory.c:1332     |

## 适用场景

✅ **适合**: 中小规模并行任务、固定线程数、单线程内存使用  
❌ **不适合**: 大规模矩阵运算、NUMA系统、极致性能需求

## 性能数据

- 单次执行: **0.06 ms** (4并行任务)
- 吞吐量: **16747 iter/s**
- 内存重用: **零开销**

## 文档说明

- `OpenBLAS_GEMM_Multithreading_Analysis.md`: OpenBLAS源码深度分析
- `IMPLEMENTATION_DETAIL.md`: 本实现完整技术细节