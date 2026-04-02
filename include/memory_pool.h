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

struct FreeObject {
  FreeObject* next = nullptr;
};

struct ObjectHeader {
  Span* span = nullptr;
};

class SizeClass {
 public:
  static constexpr std::size_t kAlign = 8;
  static constexpr std::size_t kMaxBytes = 1024;
  static constexpr std::size_t kNumClasses = kMaxBytes / kAlign;

  static std::size_t RoundUp(std::size_t bytes);
  static std::size_t Index(std::size_t bytes);
  static std::size_t NumMoveSize(std::size_t bytes);
  static std::size_t NumPages(std::size_t bytes);
};

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

struct Span {
  void* memory = nullptr;
  std::size_t bytes = 0;
  std::size_t page_count = 0;
  std::size_t object_size = 0;
  std::size_t object_count = 0;
  std::size_t use_count = 0;
  bool large = false;
  FreeObject* free_list = nullptr;
};

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

class CentralCache {
 public:
  static CentralCache& Instance();

  Span* FetchRange(void*& start,
                   void*& end,
                   std::size_t batch_num,
                   std::size_t bytes);
  void ReleaseListToSpans(void* start, std::size_t bytes);

 private:
  CentralCache() = default;
  CentralCache(const CentralCache&) = delete;
  CentralCache& operator=(const CentralCache&) = delete;

  Span* GetOneSpan(std::size_t index, std::size_t bytes);

  std::array<std::vector<Span*>, SizeClass::kNumClasses> span_lists_;
  std::array<std::mutex, SizeClass::kNumClasses> mutexes_;
};

class ThreadCache {
 public:
  void* Allocate(std::size_t bytes);
  void Deallocate(void* ptr, std::size_t bytes);

 private:
  void* FetchFromCentralCache(std::size_t index, std::size_t bytes);
  void ListTooLong(FreeList& list, std::size_t bytes);

  std::array<FreeList, SizeClass::kNumClasses> free_lists_{};
};

class MemoryPool {
 public:
  static void* Allocate(std::size_t bytes);
  static void Deallocate(void* ptr, std::size_t bytes = 0);

 private:
  static ThreadCache& LocalCache();
};

struct BenchmarkResult {
  std::string name;
  std::size_t thread_count = 0;
  std::size_t iterations = 0;
  std::size_t alloc_size = 0;
  std::chrono::milliseconds elapsed{0};
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
