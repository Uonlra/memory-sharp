#include "memory_pool.h"

#include <algorithm>
#include <iomanip>
#include <new>

namespace hpmem {

namespace {

double ToMilliseconds(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

}  // namespace

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
  // 根据指针地址偏移回退找到对象头，从而定位该对象隶属的 Span 描述结构
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
  
  // 遍历寻找一个还有空闲对象可用的 Span
  for (Span* span : spans) {
    if (span->free_list != nullptr) {
      return span;
    }
  }

  // 若当前列表中无可用 Span，向 PageCache 申请新的页
  Span* span = PageCache::Instance().NewSpan(SizeClass::NumPages(bytes));
  span->object_size = SizeClass::RoundUp(bytes);
  span->object_count = span->bytes / span->object_size;
  span->use_count = 0; // 初始未分配过对象

  char* cur = static_cast<char*>(span->memory);
  
  // 初始化刚分配的 Span，将其按对象尺寸分割成一个个空闲块
  // 并在每个空闲块的前端写入 ObjectHeader (指向自己这个 Span)
  auto* first_header = reinterpret_cast<ObjectHeader*>(cur);
  first_header->span = span;
  span->free_list = reinterpret_cast<FreeObject*>(cur + sizeof(ObjectHeader));
  FreeObject* tail = span->free_list;
  
  // 将整个 Span 切片串成自由链表
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
  
  // 对 Central Cache 对应的特定哈希桶加锁，避免多线程并发获取同一大小发生竞争
  std::lock_guard<std::mutex> guard(mutexes_[index]);

  // 从此桶中取出一个带有空闲资源的 Span
  Span* span = GetOneSpan(index, bytes);
  start = span->free_list;
  FreeObject* cur = span->free_list;
  FreeObject* prev = nullptr;
  std::size_t actual = 0;

  // 根据需求取回足够数量 (batch_num) 的对象
  while (cur != nullptr && actual < batch_num) {
    prev = cur;
    cur = cur->next;
    ++actual;
  }

  // 截断拿出去的这部分链表，同时更新剩余部分作为 Span 的 free_list
  if (prev != nullptr) {
    prev->next = nullptr;
    end = prev;
  }
  
  span->free_list = cur;
  span->use_count += actual; // 标记从这个 Span 又借出了多少小块对象
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
  // 根据申请字节数计算出向上对齐后的真实大小以及对应的哈希桶索引
  const std::size_t rounded = SizeClass::RoundUp(bytes);
  const std::size_t index = SizeClass::Index(rounded);
  
  // 优先从本线程的 ThreadCache 自由链表中获取空闲对象
  void* ptr = free_lists_[index].Pop();
  if (ptr != nullptr) {
    return ptr;
  }
  
  // 如果当前缓存为空，则向中央缓存去申请一批对象
  return FetchFromCentralCache(index, rounded);
}

void ThreadCache::Deallocate(void* ptr, std::size_t bytes) {
  // 计算对象大小及对应的桶索引
  const std::size_t rounded = SizeClass::RoundUp(bytes);
  const std::size_t index = SizeClass::Index(rounded);
  
  FreeList& list = free_lists_[index];
  list.Push(ptr); // 归还对象到本地线程缓存
  
  // 如果该链表缓冲的对象过多，则回收一部分给系统 (Central Cache) 防止内存泄漏
  if (list.Size() >= list.MaxSize() * 2) {
    ListTooLong(list, rounded);
  }
}

ThreadCache& MemoryPool::LocalCache() {
  // 只有初次访问时实例化，每个线程有自己独立的一份 ThreadCache
  thread_local ThreadCache cache;
  return cache;
}

void* MemoryPool::Allocate(std::size_t bytes) {
  if (bytes == 0) {
    bytes = 1;
  }
  
  // 加上对象头大小，以便之后释放时能够找回对应的 Span
  const std::size_t actual_bytes = bytes + sizeof(ObjectHeader);
  
  // 如果分配的大于 ThreadCache 能处理的最大容量 (例如 > 1024 / 256K等, 根据常量定)
  // 则直接向页缓存 (PageCache) 申请大图的大对象
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

  // 通过对象地址减去对象头的大小，找回该对象所属的 Span
  Span* span = PageCache::MapObjectToSpan(ptr);
  if (span == nullptr) {
    return;
  }

  // 如果是直接从页缓存分配的大对象，则直接归还给页缓存
  if (span->large) {
    PageCache::Instance().ReleaseSpan(span);
    return;
  }

  if (bytes == 0) {
    bytes = span->object_size - sizeof(ObjectHeader);
  }
  
  // 普通小对象归还到本地线程缓存中
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
          ToMilliseconds(end - start)};
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
          ToMilliseconds(end - start)};
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

    if (comparison.optimized.elapsed_ms > 0.0) {
      comparison.speedup = comparison.baseline.elapsed_ms / comparison.optimized.elapsed_ms;
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
    out << "      \"baselineMs\": " << std::fixed << std::setprecision(3)
        << item.baseline.elapsed_ms << ",\n";
    out << "      \"poolMs\": " << std::fixed << std::setprecision(3)
        << item.optimized.elapsed_ms << ",\n";
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
