#include "memory_pool.h"

#include <iomanip>
#include <iostream>
#include <vector>

namespace {

void DemoBasicUsage() {
  std::cout << "== Basic Demo ==\n";
  std::vector<void*> objects;
  for (std::size_t bytes : {16u, 24u, 64u, 128u, 512u, 1500u}) {
    void* ptr = hpmem::MemoryPool::Allocate(bytes);
    objects.push_back(ptr);
    std::cout << "allocate " << std::setw(4) << bytes << " bytes -> " << ptr << '\n';
  }

  const std::size_t sizes[] = {16u, 24u, 64u, 128u, 512u, 1500u};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    hpmem::MemoryPool::Deallocate(objects[i], sizes[i]);
  }
  std::cout << "all objects returned to pool\n\n";
}

void DemoBenchmark() {
  std::cout << "== Benchmark ==\n";
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kIterations = 200000;
  constexpr std::size_t kAllocSize = 64;

  auto baseline = hpmem::RunNewDeleteBenchmark(kThreads, kIterations, kAllocSize);
  auto pool = hpmem::RunMemoryPoolBenchmark(kThreads, kIterations, kAllocSize);

  std::cout << baseline.name << ": " << baseline.elapsed.count() << " ms\n";
  std::cout << pool.name << ": " << pool.elapsed.count() << " ms\n";
  if (pool.elapsed.count() > 0) {
    const double speedup =
        static_cast<double>(baseline.elapsed.count()) / static_cast<double>(pool.elapsed.count());
    std::cout << "speedup: " << std::fixed << std::setprecision(2) << speedup << "x\n";
  }
}

}  // namespace

int main() {
  DemoBasicUsage();
  DemoBenchmark();
  return 0;
}
