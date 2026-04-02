# 高性能内存池设计项目

这是一个适合考研复试展示的 C++ 项目，核心目标是用分级缓存的方式减少频繁 `new/delete` 带来的系统调用开销和锁竞争。

## 项目亮点

- 小对象分配采用 `ThreadCache -> CentralCache -> PageCache` 三层结构。
- 线程本地缓存优先命中，降低多线程下的共享锁竞争。
- 中心缓存按 `Span` 管理对象批量切分与回收。
- 大对象绕过小对象缓存，直接走独立 `Span`。
- 提供了简单基准测试，方便在复试时演示性能提升。
- 新增可视化展示界面，可直接展示项目成果、架构作用和性能对比。

## 目录结构

- `include/memory_pool.h`：核心接口与数据结构定义。
- `src/memory_pool.cpp`：内存池实现。
- `src/main.cpp`：演示与压测程序。
- `ui/index.html`：项目可视化展示界面。
- `ui/data/benchmark_report.json`：程序生成的性能对比报告。
- `docs/project_notes.md`：知识点清单与面试问题。
- `docs/vibe_interview.md`：按照 vibe coding 风格整理的模拟提问脚本。

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

## 可视化界面使用

先运行程序生成最新 benchmark 报告：

```powershell
.\memory_pool_demo.exe
```

然后打开下面这个页面即可查看可视化展示：

`ui/index.html`

页面会突出展示：

- 项目作用与适用场景
- 三层缓存架构与 Span 管理思路
- 默认分配器和内存池的图例对比
- 多场景 benchmark 柱状图与加速比
- 复试时可直接讲的项目成果


## 当前实现的边界

- 当前版本通过对象头记录 `Span` 归属，释放路径清晰，但还不是工业级页号映射方案。
- 跨线程释放对象时，当前实现会进入释放线程的 `ThreadCache`，这是一个可解释但仍可继续优化的点。
- 尚未实现真正的页号映射、Span 合并拆分、无锁优化和更严格的碎片控制。

这些边界并不是缺点，反而很适合复试时说明“我已经完成了核心思路落地，也清楚工业级实现还需要继续演进”。
