#include "memory_pool.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string FormatMs(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3) << value;
  return out.str();
}

std::vector<hpmem::BenchmarkCase> BuildBenchmarkCases() {
  const std::vector<std::size_t> thread_counts = {1, 2, 4, 8, 12, 16, 20, 24};
  const std::vector<std::size_t> alloc_sizes = {
      8,   16,  24,  32,  48,  64,  96,  128,
      160, 192, 256, 384, 512, 640, 768, 1024};

  std::vector<hpmem::BenchmarkCase> cases;
  cases.reserve(thread_counts.size() * alloc_sizes.size());

  for (std::size_t threads : thread_counts) {
    for (std::size_t size : alloc_sizes) {
      std::size_t iterations = 30000;
      if (size > 128) {
        iterations = 22000;
      }
      if (size > 512) {
        iterations = 16000;
      }
      cases.push_back({threads, iterations, size});
    }
  }

  return cases;
}

void DemoBasicUsage() {
  std::cout << "== Basic Demo ==\n";
  std::vector<void*> objects;
  // 按照不同的大小申请内存，演示跨越多种大小分配的需求
  for (std::size_t bytes : {16u, 24u, 64u, 128u, 512u, 1500u}) {
    void* ptr = hpmem::MemoryPool::Allocate(bytes);
    objects.push_back(ptr);
    std::cout << "allocate " << std::setw(4) << bytes << " bytes -> " << ptr << '\n';
  }

  // 释放所申请和持有的所有对象回内存池中
  const std::size_t sizes[] = {16u, 24u, 64u, 128u, 512u, 1500u};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    hpmem::MemoryPool::Deallocate(objects[i], sizes[i]);
  }
  std::cout << "all objects returned to pool\n\n";
}

void PrintSummaryMetrics(const hpmem::BenchmarkReport& report) {
  double best_speedup = 0.0;
  double avg_speedup = 0.0;
  for (const auto& item : report.comparisons) {
    best_speedup = std::max(best_speedup, item.speedup);
    avg_speedup += item.speedup;
  }
  if (!report.comparisons.empty()) {
    avg_speedup /= static_cast<double>(report.comparisons.size());
  }

  std::cout << "\n== Summary ==\n";
  std::cout << "cases: " << report.comparisons.size() << '\n';
  std::cout << "best speedup: " << std::fixed << std::setprecision(2) << best_speedup << "x\n";
  std::cout << "avg speedup : " << std::fixed << std::setprecision(2) << avg_speedup << "x\n";
}

void DemoBenchmark(const std::string& report_path) {
  std::cout << "== Benchmark ==\n";
  const std::vector<hpmem::BenchmarkCase> cases = BuildBenchmarkCases();

  const auto report = hpmem::RunBenchmarkSuite(cases);
  for (std::size_t i = 0; i < report.comparisons.size(); ++i) {
    const auto& item = report.comparisons[i];
    if (i < 8 || i + 8 >= report.comparisons.size()) {
      std::cout << '[' << item.config.thread_count << " threads, "
                << item.config.alloc_size << " bytes] ";
      std::cout << item.baseline.name << ": " << FormatMs(item.baseline.elapsed_ms) << " ms, ";
      std::cout << item.optimized.name << ": " << FormatMs(item.optimized.elapsed_ms) << " ms, ";
      std::cout << "speedup: " << std::fixed << std::setprecision(2) << item.speedup << "x\n";
    } else if (i == 8) {
      std::cout << "... " << (report.comparisons.size() - 16)
                << " more cases omitted from console output ...\n";
    }
  }

  PrintSummaryMetrics(report);

  if (!report_path.empty()) {
    std::ofstream out(report_path, std::ios::trunc);
    if (out.is_open()) {
      hpmem::WriteBenchmarkReportJson(report, out);
      std::cout << "\nreport written to: " << report_path << '\n';

      const std::size_t ext_pos = report_path.rfind(".json");
      if (ext_pos != std::string::npos) {
        const std::string js_path = report_path.substr(0, ext_pos) + ".js";
        std::ofstream js_out(js_path, std::ios::trunc);
        if (js_out.is_open()) {
          js_out << "window.BENCHMARK_REPORT = ";
          hpmem::WriteBenchmarkReportJson(report, js_out);
          js_out << ";\n";
          std::cout << "report mirror written to: " << js_path << '\n';
        }
      }
    } else {
      std::cout << "\nfailed to write report: " << report_path << '\n';
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string report_path = "ui/data/benchmark_report.json";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--no-report") {
      report_path.clear();
    } else if (arg.rfind("--report=", 0) == 0) {
      report_path = arg.substr(9);
    }
  }

  DemoBasicUsage();
  DemoBenchmark(report_path);
  return 0;
}
