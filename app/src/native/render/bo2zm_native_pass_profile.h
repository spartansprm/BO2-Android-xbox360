// SPDX-License-Identifier: GPL-3.0-only


#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <span>
#include <vector>

namespace bo2zm::native::pass_profile {
inline constexpr uint32_t kPeriod = 64, kQueries = 128, kMaxPasses = 48, kPairs = 12,
    kGroups = 128, kCategories = 9, kNone = UINT32_MAX;
inline constexpr uint64_t kWarmupNs = 10000000000ull;
inline std::atomic<bool> enabled{false}, supported{false}, foreground{true};
inline std::atomic<uint64_t> generation{1};


inline std::atomic<bool> serialize{false};
inline void Foreground(bool on) {
  if (foreground.exchange(on, std::memory_order_acq_rel) != on)
    generation.fetch_add(1, std::memory_order_acq_rel);
}
struct Pair { uint32_t vs = 0, ps = 0; uint64_t draws = 0, elements = 0; };
struct Meta {
  uint32_t category = 0, width = 0, height = 0, ordinal = 0;
  uint64_t target = 0;
  std::array<uint32_t, 5> formats{};
  uint32_t load = 0;
  bool operator==(const Meta&) const = default;
};
struct Pass {
  Meta meta;
  uint32_t begin = kNone, end = kNone;
  uint64_t draws = 0, elements = 0, untracked_pair_draws = 0;
  std::array<Pair, kPairs> pairs{};
};
struct Job {
  bool observe = false, sampled = false, truncated = false;
  uint64_t generation = 0, serial = 0;
  uint32_t count = 0, active = kNone, seen = 0;
  std::array<Pass, kMaxPasses> passes{};

  bool Begin(Meta meta, uint32_t marker_count) {
    if (!sampled) return false;
    meta.ordinal = seen++;
    if (active != kNone || count == kMaxPasses || marker_count + 3 > kQueries) {
      truncated = true; return false;
    }
    active = count++;
    auto& p = passes[active];p = {};p.meta = meta;p.begin = marker_count;
    return true;
  }
  bool End(uint32_t marker_count) {
    if (active == kNone) return false;
    if (marker_count + 2 > kQueries) { truncated = true; active = kNone; return false; }
    passes[active].end = marker_count;active = kNone;return true;
  }
  void Draw(uint32_t vs, uint32_t ps, uint32_t elements) {
    if (active == kNone) return;
    auto& p = passes[active];++p.draws;p.elements += elements;
    for (auto& pair : p.pairs) if (pair.draws && pair.vs == vs && pair.ps == ps) {
      ++pair.draws;pair.elements += elements;return;
    }
    for (auto& pair : p.pairs) if (!pair.draws) { pair = {vs,ps,1,elements};return; }
    ++p.untracked_pair_draws;
  }
};
class Scheduler {
 public:
  Job Next(uint64_t now, bool allowed, uint64_t epoch) {
    Job job;
    if (!allowed) { ready_ = 0; eligible_ = 0; epoch_ = epoch;return job; }
    if (!ready_ || epoch != epoch_) { ready_ = now;eligible_ = 0;epoch_ = epoch;return job; }
    if (now < ready_ || now - ready_ < kWarmupNs) return job;
    job.observe = true;job.generation = epoch;job.serial = ++eligible_;
    job.sampled = eligible_ % kPeriod == 0;
    return job;
  }
 private:
  uint64_t ready_ = 0, eligible_ = 0, epoch_ = 0;
};
struct Group {
  Meta meta;
  uint64_t samples = 0, gpu_ns = 0, max_ns = 0, draws = 0, elements = 0, untracked_pair_draws = 0;
  std::array<Pair, kPairs> pairs{};
};
struct Totals {
  uint64_t sampled_jobs = 0, baseline_jobs = 0, failed_jobs = 0, discarded_jobs = 0,
      truncated_jobs = 0, sampled_gpu_ns = 0, baseline_gpu_ns = 0, covered_gpu_ns = 0,
      unmatched_group_ns = 0;
  uint32_t groups = 0;
  std::array<uint64_t, kCategories> category_ns{};
  std::array<Group, kGroups> passes{};
};
inline std::mutex mutex;
inline Totals totals;
inline bool Add(Totals& t, const Job& job, std::span<const uint64_t> marks,
                std::span<const uint8_t> categories, double period_ns,
                bool valid_context, bool valid_queries) {
  if (!job.observe) return false;
  if (job.count > kMaxPasses || marks.size() > kQueries) { ++t.failed_jobs;return false; }
  if (!valid_context) { ++t.discarded_jobs;return false; }
  if (!valid_queries || marks.size() < 2 || marks.size() != categories.size() ||
      !std::isfinite(period_ns) || period_ns <= 0 ||
      !std::is_sorted(marks.begin(), marks.end())) { ++t.failed_jobs;return false; }
  const double elapsed=double(marks.back()-marks.front())*period_ns;
  if (!std::isfinite(elapsed) || elapsed >= double(UINT64_MAX)) { ++t.failed_jobs;return false; }
  const auto ns=[&](uint64_t ticks){return uint64_t(double(ticks)*period_ns);};
  const uint64_t total_ns = ns(marks.back()-marks.front());
  if (!job.sampled) { ++t.baseline_jobs;t.baseline_gpu_ns += total_ns;return true; }

  uint32_t previous_end = 0;
  for (uint32_t i=0;i<job.count;++i) {
    const auto& p=job.passes[i];
    if (p.begin == kNone || p.end == kNone || p.begin >= p.end || p.end >= marks.size()-1 ||
        (i && p.begin < previous_end)) { ++t.failed_jobs;return false; }
    previous_end=p.end;
  }
  ++t.sampled_jobs;t.sampled_gpu_ns += total_ns;
  if (job.truncated) ++t.truncated_jobs;
  for (size_t i=0;i+1<marks.size();++i)
    if (categories[i]<kCategories)t.category_ns[categories[i]]+=ns(marks[i+1]-marks[i]);
  for (uint32_t i=0;i<job.count;++i) {
    const auto& p=job.passes[i];const uint64_t duration=ns(marks[p.end]-marks[p.begin]);
    t.covered_gpu_ns+=duration;
    Group* group=nullptr;
    for(uint32_t n=0;n<t.groups;++n)if(t.passes[n].meta==p.meta){group=&t.passes[n];break;}
    if(!group && t.groups<kGroups){group=&t.passes[t.groups++];group->meta=p.meta;}
    if(!group){t.unmatched_group_ns+=duration;continue;}
    ++group->samples;group->gpu_ns+=duration;group->max_ns=std::max(group->max_ns,duration);
    group->draws+=p.draws;group->elements+=p.elements;group->untracked_pair_draws+=p.untracked_pair_draws;
    for(const auto& pair:p.pairs)if(pair.draws){
      bool found=false;
      for(auto& q:group->pairs)if(q.draws && q.vs==pair.vs && q.ps==pair.ps){q.draws+=pair.draws;q.elements+=pair.elements;found=true;break;}
      if(!found)for(auto& q:group->pairs)if(!q.draws){q=pair;found=true;break;}
      if(!found)group->untracked_pair_draws+=pair.draws;
    }
  }
  return true;
}
inline void Complete(const Job& job, std::span<const uint64_t> marks,
                     std::span<const uint8_t> categories, double period_ns,
                     bool valid_context, bool valid_queries) {
  if(!job.observe)return;
  std::lock_guard lock(mutex);Add(totals,job,marks,categories,period_ns,valid_context,valid_queries);
}
inline const char* Category(uint32_t n) {
  constexpr const char* names[]={"other_job_work","depth_only_large_target","scene_with_depth",
      "medium_targets","small_targets_320_plus","small_targets_under_320","copies","clears","full_width_without_depth"};
  return n<kCategories?names[n]:"unknown";
}
inline std::string Json() {
  Totals t;
  {std::lock_guard lock(mutex);t=totals;}
  std::vector<const Group*> rank;
  for(uint32_t i=0;i<t.groups;++i)rank.push_back(&t.passes[i]);
  std::sort(rank.begin(),rank.end(),[](auto a,auto b){return a->gpu_ns>b->gpu_ns;});
  uint64_t other_tracked_pass_gpu_ns=0;
  for(size_t i=8;i<rank.size();++i)other_tracked_pass_gpu_ns+=rank[i]->gpu_ns;
  std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(8);
  s<<"{\"schema\":1,\"enabled\":"<<(enabled.load()?"true":"false")
   <<",\"supported\":"<<(supported.load()?"true":"false")<<",\"foreground\":"<<(foreground.load()?"true":"false")
   <<",\"sample_period_jobs\":"<<kPeriod<<",\"warmup_seconds\":10,\"sampled_jobs\":"<<t.sampled_jobs
   <<",\"baseline_jobs\":"<<t.baseline_jobs<<",\"failed_jobs\":"<<t.failed_jobs<<",\"discarded_jobs\":"<<t.discarded_jobs
   <<",\"truncated_jobs\":"<<t.truncated_jobs<<",\"sampled_gpu_ns\":"<<t.sampled_gpu_ns
   <<",\"baseline_gpu_ns\":"<<t.baseline_gpu_ns<<",\"pass_gpu_ns\":"<<t.covered_gpu_ns
   <<",\"outside_pass_gpu_ns\":"<<(t.sampled_gpu_ns>=t.covered_gpu_ns?t.sampled_gpu_ns-t.covered_gpu_ns:0)
   <<",\"untracked_group_gpu_ns\":"<<t.unmatched_group_ns
   <<",\"other_tracked_pass_gpu_ns\":"<<other_tracked_pass_gpu_ns<<",\"tracked_pass_groups\":"<<t.groups
   <<",\"category_scope\":\"target-size heuristics; only sampled job buffers; Android presenter excluded\""
   <<",\"shader_scope\":\"logical draw counts inside pass; not individual shader GPU timings\""
   <<",\"timing_scope\":\"BOTTOM_OF_PIPE elapsed intervals; diagnostic markers may perturb sampled jobs; not GPU utilization\""
   <<",\"categories\":[";
  for(uint32_t i=0;i<kCategories;++i){if(i)s<<',';s<<"{\"id\":"<<i<<",\"name\":\""<<Category(i)<<"\",\"gpu_ns\":"<<t.category_ns[i]<<'}';}
  s<<"],\"top_passes\":[";
  for(size_t i=0;i<std::min<size_t>(8,rank.size());++i){
    const auto& g=*rank[i];if(i)s<<',';
    s<<"{\"target_hash\":\""<<std::hex<<g.meta.target<<std::dec<<"\",\"ordinal\":"<<g.meta.ordinal
     <<",\"category\":"<<g.meta.category<<",\"width\":"<<g.meta.width<<",\"height\":"<<g.meta.height
     <<",\"load_mode\":"<<g.meta.load<<",\"serialized\":"<<(serialize.load()?"true":"false")
     <<",\"formats\":[";
    for(uint32_t f=0;f<5;++f){if(f)s<<',';s<<g.meta.formats[f];}
    s<<"],\"samples\":"<<g.samples<<",\"gpu_ns\":"<<g.gpu_ns<<",\"max_gpu_ns\":"<<g.max_ns
     <<",\"logical_draws\":"<<g.draws<<",\"elements\":"<<g.elements<<",\"untracked_pair_draws\":"<<g.untracked_pair_draws
     <<",\"shader_pairs\":[";
    auto pairs=g.pairs;std::sort(pairs.begin(),pairs.end(),[](auto&a,auto&b){return a.draws>b.draws;});
    for(uint32_t p=0;p<3 && pairs[p].draws;++p){if(p)s<<',';s<<"{\"vs\":"<<pairs[p].vs<<",\"ps\":"<<pairs[p].ps
      <<",\"logical_draws\":"<<pairs[p].draws<<",\"elements\":"<<pairs[p].elements<<'}';}
    s<<"]}";
  }
  return s.str()+"]}";
}
}
