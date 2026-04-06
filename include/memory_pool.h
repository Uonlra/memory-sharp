#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace hpmem {

struct Span;

// 自由链表节点，用于将空闲内存块串联成侵入式链表
struct FreeObject {
  FreeObject* next = nullptr;
};

// 对象头，用于帮助内存块快速找到其所属的 Span
struct ObjectHeader {
  Span* span = nullptr;
};

// 内存大小和对齐的工具类
class SizeClass {
 public:
  static constexpr std::size_t kAlign = 8; // 基础对齐大小
  static constexpr std::size_t kMaxBytes = 1024; // 线程缓存处理的最大内存大小
  static constexpr std::size_t kNumClasses = kMaxBytes / kAlign; // 自由链表的数量

  static std::size_t RoundUp(std::size_t bytes);
  static std::size_t Index(std::size_t bytes);
  static std::size_t NumMoveSize(std::size_t bytes);
  static std::size_t NumPages(std::size_t bytes);
};

// 自由链表，用于管理相同大小的空闲内存块
class FreeList {
 public:
  void Push(void* ptr);
  void PushRange(void* start, void* end, std::size_t count);
  void* Pop();
  void PopRange(void*& start, void*& end, std::size_t count);

  std::size_t Size() const { return size_; }
  bool Empty() const { return head_ == nullptr; }
  void SetMaxSize(std::size_t max_size) { max_size_ = max_size; }
  std::size_t MaxSize() const { return max_size_; }

 private:
  FreeObject* head_ = nullptr;
  std::size_t size_ = 0;
  std::size_t max_size_ = 1;
};

// 跨越连续内存页的区块，可以划分出多个固定大小的内存对象
struct Span {
  void* memory = nullptr;      // 该 Span 管理的连续内存块起始地址
  std::size_t bytes = 0;       // 分配的内存总字节数
  std::size_t page_count = 0;  // 页数
  std::size_t object_size = 0; // 单个内存对象的大小
  std::size_t object_count = 0;// 内存对象总数
  std::size_t use_count = 0;   // 已经使用（分配出去）的对象数量
  bool large = false;          // 标记该 Span 是否用于管理大对象 (>1024 字节)
  FreeObject* free_list = nullptr; // 指向空闲对象链表的头部
};

// 页缓存，管理系统中分配的大块内存页面，属于全局共享层面
class PageCache {
 public:
  static constexpr std::size_t kPageSize = 4096;

  static PageCache& Instance();

  Span* NewSpan(std::size_t page_count);
  Span* NewLargeSpan(std::size_t bytes);
  void ReleaseSpan(Span* span);
  static Span* MapObjectToSpan(void* ptr);

 private:
  PageCache() = default;
  PageCache(const PageCache&) = delete;
  PageCache& operator=(const PageCache&) = delete;
};

// 中央缓存（中心映射层），缓存由 PageCache 提供的大内存块切割后的小块对象对象池
class CentralCache {
 public:
  static CentralCache& Instance();

  // 从 CentralCache 中批量获取对象提供给 ThreadCache
  Span* FetchRange(void*& start,
                   void*& end,
                   std::size_t batch_num,
                   std::size_t bytes);
                   
  // 将过多的空闲对象归还至对应的 Span 中
  void ReleaseListToSpans(void* start, std::size_t bytes);

 private:
  CentralCache() = default;
  CentralCache(const CentralCache&) = delete;
  CentralCache& operator=(const CentralCache&) = delete;

  Span* GetOneSpan(std::size_t index, std::size_t bytes);

  // 为每个大小类维护对应的 Span 集合以及互斥锁
  std::array<std::vector<Span*>, SizeClass::kNumClasses> span_lists_;
  std::array<std::mutex, SizeClass::kNumClasses> mutexes_;
};

// 线程级缓存，每个线程通过 ThreadLocal 拥有独立的缓存实例，实现无锁的极速分配
class ThreadCache {
 public:
  void* Allocate(std::size_t bytes);  // 线程级申请内存小对象
  void Deallocate(void* ptr, std::size_t bytes); // 归还小对象到当前线程缓存

 private:
  void* FetchFromCentralCache(std::size_t index, std::size_t bytes);
  void ListTooLong(FreeList& list, std::size_t bytes);

  // 为每个大小类别维护独立的自由空闲链表
  std::array<FreeList, SizeClass::kNumClasses> free_lists_{};
};

// 内存池统一入口
class MemoryPool {
 public:
  static void* Allocate(std::size_t bytes);
  static void Deallocate(void* ptr, std::size_t bytes = 0);

 private:
  // 获取当前线程局部存储的 ThreadCache 实例
  static ThreadCache& LocalCache();
};

struct BenchmarkResult {
  std::string name;
  std::size_t thread_count = 0;
  std::size_t iterations = 0;
  std::size_t alloc_size = 0;
  double elapsed_ms = 0.0;
};

struct BenchmarkCase {
  std::size_t thread_count = 0;
  std::size_t iterations = 0;
  std::size_t alloc_size = 0;
};

struct BenchmarkComparison {
  BenchmarkCase config;
  BenchmarkResult baseline;
  BenchmarkResult optimized;
  double speedup = 0.0;
};

struct BenchmarkReport {
  std::vector<BenchmarkComparison> comparisons;
};

BenchmarkResult RunNewDeleteBenchmark(std::size_t thread_count,
                                      std::size_t iterations,
                                      std::size_t alloc_size);

BenchmarkResult RunMemoryPoolBenchmark(std::size_t thread_count,
                                       std::size_t iterations,
                                       std::size_t alloc_size);

BenchmarkReport RunBenchmarkSuite(const std::vector<BenchmarkCase>& cases);
void WriteBenchmarkReportJson(const BenchmarkReport& report, std::ostream& out);

}  // namespace hpmem
