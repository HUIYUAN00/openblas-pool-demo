# 线程池与内存池上下文

OpenBLAS风格的高性能线程池与内存池实现，参考OpenBLAS源码的核心优化技术。

## Language

**Thread Pool（线程池）**：
管理一组工作线程，通过原子队列分发任务，支持自旋等待优化。
_Avoid_: worker pool, executor pool

**Memory Pool（内存池）**：
基于TLS或预分配模式的内存分配器，提供64字节对齐的缓冲区。
_Avoid_: buffer pool, allocator

**Task Queue（任务队列）**：
任务函数及其参数的队列结构，每线程独立队列。
_Avoid_: job queue, work queue

**Spin Wait（自旋等待）**：
短任务快速响应策略，YIELDING指令+rpcc()超时检测，避免上下文切换。
_Avoid_: busy wait, spin lock

**TLS（Thread Local Storage）**：
线程本地存储，实现无锁内存分配，每线程独立缓冲区池。
_Avoid_: thread-local cache, per-thread buffer

**Adapter（适配器）**：
在接缝处满足接口的具体实现（如预分配适配器、TLS适配器）。
_Avoid_: implementation, strategy

## Relationships

- **Thread Pool** 拥有 **Task Queue**（每线程一个）
- **Memory Pool** 通过 **Adapter** 实现（预分配或TLS）
- **Thread Pool** 使用 **Spin Wait** 策略等待任务
- **Memory Pool** 提供64字节对齐的缓冲区给调用者