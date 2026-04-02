#include "memory_pool.h"

#include <algorithm>
#include <iomanip>
#include <new>

namespace hpmem {

std::size_t SizeClass::RoundUp(std::size_t bytes) {
  if (bytes == 0) {
    return kAlign;
  }
  if (bytes > kMaxBytes) {
    return bytes;
  }
  return (bytes + kAlign - 1) & ~(kAlign - 1);
}

std::size_t SizeClass::Index(std::size_t bytes) {
  const std::size_t rounded = RoundUp(bytes);
  return rounded / kAlign - 1;
}

std::size_t SizeClass::NumMoveSize(std::size_t bytes) {
  std::size_t num = 64 * 1024 / RoundUp(bytes);
  num = std::clamp<std::size_t>(num, 2, 512);
  return num;
}

std::size_t SizeClass::NumPages(std::size_t bytes) {
  const std::size_t batch_bytes = NumMoveSize(bytes) * RoundUp(bytes);
  std::size_t pages = (batch_bytes + PageCache::kPageSize - 1) / PageCache::kPageSize;
  return std::max<std::size_t>(1, pages);
}

void FreeList::Push(void* ptr) {
  auto* obj = static_cast<FreeObject*>(ptr);
  obj->next = head_;
  head_ = obj;
  ++size_;
}

void FreeList::PushRange(void* start, void* end, std::size_t count) {
  if (start == nullptr) {
    return;
  }
  auto* start_obj = static_cast<FreeObject*>(start);
  auto* end_obj = static_cast<FreeObject*>(end);
  end_obj->next = head_;
  head_ = start_obj;
  size_ += count;
}

void* FreeList::Pop() {
  if (head_ == nullptr) {
    return nullptr;
  }
  FreeObject* obj = head_;
  head_ = obj->next;
  obj->next = nullptr;
  --size_;
  return obj;
}

void FreeList::PopRange(void*& start, void*& end, std::size_t count) {
  start = nullptr;
  end = nullptr;
  if (count == 0 || head_ == nullptr) {
    return;
  }

  start = head_;
  FreeObject* cur = head_;
  std::size_t actual = 1;
  while (actual < count && cur->next != nullptr) {
    cur = cur->next;
    ++actual;
  }
  end = cur;
  head_ = cur->next;
  cur->next = nullptr;
  size_ -= actual;
}

PageCache& PageCache::Instance() {
  static PageCache cache;
  return cache;
}

Span* PageCache::NewSpan(std::size_t page_count) {
  auto* span = new Span();
  span->page_count = page_count;
  span->bytes = page_count * kPageSize;
  span->memory = ::operator new(span->bytes, std::align_val_t{kPageSize});
  return span;
}

Span* PageCache::NewLargeSpan(std::size_t bytes) {
  auto* span = new Span();
  span->large = true;
  span->object_size = SizeClass::RoundUp(bytes + sizeof(ObjectHeader));
  span->bytes = span->object_size;
  span->page_count = (span->bytes + kPageSize - 1) / kPageSize;
  span->memory = ::operator new(span->bytes);
  auto* header = static_cast<ObjectHeader*>(span->memory);
  header->span = span;
  return span;
}

void PageCache::ReleaseSpan(Span* span) {
  if (span == nullptr) {
    return;
  }

  if (!span->large && span->memory != nullptr) {
    ::operator delete(span->memory, std::align_val_t{kPageSize});
  } else if (span->memory != nullptr) {
    ::operator delete(span->memory);
  }

  delete span;
}

Span* PageCache::MapObjectToSpan(void* ptr) {
  auto* header =
      reinterpret_cast<ObjectHeader*>(static_cast<char*>(ptr) - sizeof(ObjectHeader));
  return header->span;
}

CentralCache& CentralCache::Instance() {
  static CentralCache cache;
  return cache;
}

Span* CentralCache::GetOneSpan(std::size_t index, std::size_t bytes) {
  auto& spans = span_lists_[index];
  for (Span* span : spans) {
    if (span->free_list != nullptr) {
      return span;
    }
  }

  Span* span = PageCache::Instance().NewSpan(SizeClass::NumPages(bytes));
  span->object_size = SizeClass::RoundUp(bytes);
  span->object_count = span->bytes / span->object_size;
  span->use_count = 0;

  char* cur = static_cast<char*>(span->memory);
  auto* first_header = reinterpret_cast<ObjectHeader*>(cur);
  first_header->span = span;
  span->free_list = reinterpret_cast<FreeObject*>(cur + sizeof(ObjectHeader));
  FreeObject* tail = span->free_list;
  for (std::size_t i = 1; i < span->object_count; ++i) {
    cur += span->object_size;
    auto* header = reinterpret_cast<ObjectHeader*>(cur);
    header->span = span;
    tail->next = reinterpret_cast<FreeObject*>(cur + sizeof(ObjectHeader));
    tail = tail->next;
  }
  tail->next = nullptr;

  spans.push_back(span);
  return span;
}

Span* CentralCache::FetchRange(void*& start,
                               void*& end,
                               std::size_t batch_num,
                               std::size_t bytes) {
  start = nullptr;
  end = nullptr;
  const std::size_t index = SizeClass::Index(bytes);
  std::lock_guard<std::mutex> guard(mutexes_[index]);

  Span* span = GetOneSpan(index, bytes);
  start = span->free_list;
  FreeObject* cur = span->free_list;
  FreeObject* prev = nullptr;
  std::size_t actual = 0;

  while (cur != nullptr && actual < batch_num) {
    prev = cur;
    cur = cur->next;
    ++actual;
  }

  if (prev != nullptr) {
    prev->next = nullptr;
    end = prev;
  }
  span->free_list = cur;
  span->use_count += actual;
  return span;
}

void CentralCache::ReleaseListToSpans(void* start, std::size_t bytes) {
  if (start == nullptr) {
    return;
  }

  const std::size_t index = SizeClass::Index(bytes);
  std::lock_guard<std::mutex> guard(mutexes_[index]);

  FreeObject* cur = static_cast<FreeObject*>(start);
  while (cur != nullptr) {
    FreeObject* next = cur->next;
    Span* span = PageCache::MapObjectToSpan(cur);
    if (span == nullptr) {
      cur = next;
      continue;
    }

    cur->next = span->free_list;
    span->free_list = cur;
    if (span->use_count > 0) {
      --span->use_count;
    }
    if (span->use_count == 0) {
      auto& spans = span_lists_[index];
      spans.erase(std::remove(spans.begin(), spans.end(), span), spans.end());
      PageCache::Instance().ReleaseSpan(span);
    }
    cur = next;
  }
}

void* ThreadCache::FetchFromCentralCache(std::size_t index, std::size_t bytes) {
  void* start = nullptr;
  void* end = nullptr;
  const std::size_t batch_num = SizeClass::NumMoveSize(bytes);
  Span* span = CentralCache::Instance().FetchRange(start, end, batch_num, bytes);
  (void)span;
  if (start == nullptr) {
    return nullptr;
  }

  FreeList& list = free_lists_[index];
  if (list.MaxSize() < batch_num) {
    list.SetMaxSize(batch_num);
  }

  FreeObject* next = static_cast<FreeObject*>(start)->next;
  static_cast<FreeObject*>(start)->next = nullptr;
  if (next != nullptr) {
    std::size_t count = 0;
    FreeObject* tail = next;
    count = 1;
    while (tail->next != nullptr) {
      tail = tail->next;
      ++count;
    }
    list.PushRange(next, tail, count);
  }
  return start;
}

void ThreadCache::ListTooLong(FreeList& list, std::size_t bytes) {
  void* start = nullptr;
  void* end = nullptr;
  const std::size_t release_num = list.Size() / 2;
  list.PopRange(start, end, release_num);
  CentralCache::Instance().ReleaseListToSpans(start, bytes);
}

void* ThreadCache::Allocate(std::size_t bytes) {
  const std::size_t rounded = SizeClass::RoundUp(bytes);
  const std::size_t index = SizeClass::Index(rounded);
  void* ptr = free_lists_[index].Pop();
  if (ptr != nullptr) {
    return ptr;
  }
  return FetchFromCentralCache(index, rounded);
}

void ThreadCache::Deallocate(void* ptr, std::size_t bytes) {
  const std::size_t rounded = SizeClass::RoundUp(bytes);
  const std::size_t index = SizeClass::Index(rounded);
  FreeList& list = free_lists_[index];
  list.Push(ptr);
  if (list.Size() >= list.MaxSize() * 2) {
    ListTooLong(list, rounded);
  }
}

ThreadCache& MemoryPool::LocalCache() {
  thread_local ThreadCache cache;
  return cache;
}

void* MemoryPool::Allocate(std::size_t bytes) {
  if (bytes == 0) {
    bytes = 1;
  }
  const std::size_t actual_bytes = bytes + sizeof(ObjectHeader);
  if (actual_bytes > SizeClass::kMaxBytes) {
    Span* span = PageCache::Instance().NewLargeSpan(bytes);
    return static_cast<char*>(span->memory) + sizeof(ObjectHeader);
  }
  return LocalCache().Allocate(actual_bytes);
}

void MemoryPool::Deallocate(void* ptr, std::size_t bytes) {
  if (ptr == nullptr) {
    return;
  }

  Span* span = PageCache::MapObjectToSpan(ptr);
  if (span == nullptr) {
    return;
  }

  if (span->large) {
    PageCache::Instance().ReleaseSpan(span);
    return;
  }

  if (bytes == 0) {
    bytes = span->object_size - sizeof(ObjectHeader);
  }
  LocalCache().Deallocate(ptr, bytes + sizeof(ObjectHeader));
}

BenchmarkResult RunNewDeleteBenchmark(std::size_t thread_count,
                                      std::size_t iterations,
                                      std::size_t alloc_size) {
  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t i = 0; i < thread_count; ++i) {
    threads.emplace_back([iterations, alloc_size]() {
      std::vector<void*> ptrs;
      ptrs.reserve(128);
      for (std::size_t j = 0; j < iterations; ++j) {
        void* ptr = ::operator new(alloc_size);
        ptrs.push_back(ptr);
        if (ptrs.size() >= 64) {
          for (void* p : ptrs) {
            ::operator delete(p);
          }
          ptrs.clear();
        }
      }
      for (void* p : ptrs) {
        ::operator delete(p);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  auto end = std::chrono::steady_clock::now();
  return {"new/delete",
          thread_count,
          iterations,
          alloc_size,
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start)};
}

BenchmarkResult RunMemoryPoolBenchmark(std::size_t thread_count,
                                       std::size_t iterations,
                                       std::size_t alloc_size) {
  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t i = 0; i < thread_count; ++i) {
    threads.emplace_back([iterations, alloc_size]() {
      std::vector<void*> ptrs;
      ptrs.reserve(128);
      for (std::size_t j = 0; j < iterations; ++j) {
        void* ptr = MemoryPool::Allocate(alloc_size);
        ptrs.push_back(ptr);
        if (ptrs.size() >= 64) {
          for (void* p : ptrs) {
            MemoryPool::Deallocate(p, alloc_size);
          }
          ptrs.clear();
        }
      }
      for (void* p : ptrs) {
        MemoryPool::Deallocate(p, alloc_size);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  auto end = std::chrono::steady_clock::now();
  return {"memory pool",
          thread_count,
          iterations,
          alloc_size,
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start)};
}

BenchmarkReport RunBenchmarkSuite(const std::vector<BenchmarkCase>& cases) {
  BenchmarkReport report;
  report.comparisons.reserve(cases.size());

  for (const auto& config : cases) {
    BenchmarkComparison comparison;
    comparison.config = config;
    comparison.baseline =
        RunNewDeleteBenchmark(config.thread_count, config.iterations, config.alloc_size);
    comparison.optimized =
        RunMemoryPoolBenchmark(config.thread_count, config.iterations, config.alloc_size);

    if (comparison.optimized.elapsed.count() > 0) {
      comparison.speedup =
          static_cast<double>(comparison.baseline.elapsed.count()) /
          static_cast<double>(comparison.optimized.elapsed.count());
    }
    report.comparisons.push_back(comparison);
  }

  return report;
}

void WriteBenchmarkReportJson(const BenchmarkReport& report, std::ostream& out) {
  out << "{\n";
  out << "  \"project\": \"High Performance Memory Pool\",\n";
  out << "  \"comparisons\": [\n";

  for (std::size_t i = 0; i < report.comparisons.size(); ++i) {
    const auto& item = report.comparisons[i];
    out << "    {\n";
    out << "      \"threads\": " << item.config.thread_count << ",\n";
    out << "      \"iterations\": " << item.config.iterations << ",\n";
    out << "      \"allocSize\": " << item.config.alloc_size << ",\n";
    out << "      \"baselineMs\": " << item.baseline.elapsed.count() << ",\n";
    out << "      \"poolMs\": " << item.optimized.elapsed.count() << ",\n";
    out << "      \"speedup\": " << std::fixed << std::setprecision(2) << item.speedup << "\n";
    out << "    }";
    if (i + 1 != report.comparisons.size()) {
      out << ",";
    }
    out << "\n";
  }

  out << "  ]\n";
  out << "}\n";
}

}  // namespace hpmem
