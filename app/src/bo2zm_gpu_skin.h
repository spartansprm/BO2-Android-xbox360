#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace bo2zm::gpu_skin {

struct Job {
  uint32_t out = 0;
  uint32_t count = 0;
  uint32_t kernel = 0;
  const uint8_t* vertices = nullptr;
  uint32_t vertices_guest = 0;
  const uint8_t* records = nullptr;
  uint8_t* target = nullptr;
  uint32_t records_guest = 0;
  uint32_t bones_guest = 0;
  uint32_t bones_lo = 0;

  uint32_t constants[32] = {};
  std::vector<uint8_t> bones;
  bool cpu_written = false;
  uint64_t serial = 0;


  mutable std::atomic<bool> forgotten{false};
  std::atomic<uint32_t> refs{0};
};


class JobRef {
 public:
  JobRef() = default;
  JobRef(const JobRef& other) : job_(other.job_) { Retain(); }
  JobRef(JobRef&& other) noexcept : job_(other.job_) { other.job_ = nullptr; }
  JobRef& operator=(const JobRef& other) {
    if (this != &other) {
      Release();
      job_ = other.job_;
      Retain();
    }
    return *this;
  }
  JobRef& operator=(JobRef&& other) noexcept {
    if (this != &other) {
      Release();
      job_ = other.job_;
      other.job_ = nullptr;
    }
    return *this;
  }
  ~JobRef() { Release(); }
  Job* operator->() const { return job_; }
  Job& operator*() const { return *job_; }
  Job* get() const { return job_; }
  explicit operator bool() const { return job_ != nullptr; }
  bool operator==(const JobRef& other) const { return job_ == other.job_; }

  static JobRef New();

 private:
  void Retain() {
    if (job_) {
      job_->refs.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void Release();
  Job* job_ = nullptr;
};


bool Enabled();

bool WantedNow();

bool CheckNext();

bool Physical(uint32_t guest, uint32_t& physical);

void Forget(uint32_t guest, uint32_t bytes);

void Add(JobRef job);

void CountNotRecorded();


bool RendererOn();

void SetRendererReady(bool ready);

bool MayOverlap(uint32_t physical, uint32_t bytes);

void Overlapping(uint32_t physical, uint32_t bytes, std::vector<JobRef>& jobs);


void Materialize(const Job& job);


struct RendererCounts {
  std::atomic<uint64_t> ranges{0};
  std::atomic<uint64_t> dispatches{0};
  std::atomic<uint64_t> vertices{0};
  std::atomic<uint64_t> gap_bytes{0};
  std::atomic<uint64_t> refused{0};
  std::atomic<uint64_t> refused_vertices{0};
  std::atomic<uint64_t> input_bytes{0};
  std::atomic<uint64_t> cache_hits{0};
  std::atomic<uint64_t> cache_fills{0};
  std::atomic<uint64_t> cache_fill_bytes{0};
  std::atomic<uint64_t> uncached{0};
  std::atomic<uint64_t> guard_fails{0};
  std::atomic<uint64_t> submit_dispatches{0};
  std::atomic<uint64_t> ns{0};

  std::atomic<uint64_t> checked_vertices{0};
  std::atomic<uint64_t> words_same{0};
  std::atomic<uint64_t> words_near{0};
  std::atomic<uint64_t> words_far{0};
};
RendererCounts& Counts();

void Refused(const char* reason, uint32_t physical, uint32_t bytes, uint32_t stride, uint32_t order);


void Compare(const uint8_t* gpu, const uint8_t* cpu, uint32_t vertices, uint32_t kernel, uint32_t physical);


void Report();

}
