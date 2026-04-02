# 高性能内存池设计项目

一个C++ 项目，核心目标是用分级缓存的方式减少频繁 `new/delete` 带来的系统调用开销和锁竞争。

## 项目亮点

- 小对象分配采用 `ThreadCache -> CentralCache -> PageCache` 三层结构。
- 线程本地缓存优先命中，降低多线程下的共享锁竞争。
- 中心缓存按 `Span` 管理对象批量切分与回收。
- 大对象绕过小对象缓存，直接走独立 `Span`。
- 提供了简单基准测试，方便在复试时演示性能提升。

## 目录结构

- `include/memory_pool.h`：核心接口与数据结构定义。
- `src/memory_pool.cpp`：内存池实现。
- `src/main.cpp`：演示与压测程序。

## 如何构建

### 方式一：直接使用 Makefile

```powershell
make
.\memory_pool_demo.exe
```

### 方式二：使用 CMake

```powershell
cmake -S . -B build
cmake --build build
.\build\memory_pool_demo.exe
```
