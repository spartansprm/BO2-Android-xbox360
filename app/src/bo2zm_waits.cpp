#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "bo2zm_benchmark.h"
#include "bo2zm_jobs.h"
#include "native/render/bo2zm_native_render.h"


REXCVAR_DEFINE_BOOL(bo2zm_blocking_waits, true, "BO2",
                    "Game threads with nothing to run in the job system sleep until a job, a job's end, the "
                    "Backend's events or ring progress may let them go on, instead of yielding in a loop. "
                    "false = the game's own spinning (and, on the Switch, bd67688's short Backend sleep)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_waits_max_us, 2000, "BO2",
                     "Longest sleep of a blocking job-system wait, in microseconds (an idle worker sleeps up to four "
                     "times this); afterwards the game checks again")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_waits_own_locks, true, "BO2",
                    "Blocking job-system waits: each sleeping thread waits on a lock and condition of its own, and a "
                    "notifier takes only the locks of the threads it wakes. false = one lock for every sleeper and "
                    "notifier, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_waits_own_locks_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_waits_own_locks for the sleeps that start "
                    "in each arm (on = a lock of their own)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_waits_cull, true, "BO2",
                    "Blocking job-system waits: a job that waits for the culling jobs (sub_828DED00) sleeps until one "
                    "of them ends instead of being put back and picked again at once. false = the game's own spinning")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_waits_cull_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_waits_cull. 0 = not part "
                     "of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(bo2zm_wait_sites, 0xFFF, "BO2",
                     "Which blocking waits are on, one bit each (diagnostics): 1 frame poll, 2 done poll, 4 ring "
                     "poll, 8 after the device lock is released, 16 workers, 32 type waits, 64 group waits, "
                     "128 the Server, 256 jobs whose dependency is not met, 512 workers that find critical "
                     "section 6 taken, 1024 threads whose last jobs were all put back, 2048 the wait for the culling "
                     "jobs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8284AED8);
REX_EXTERN(__imp__sub_827D8DD8);
REX_EXTERN(__imp__sub_8284A088);
REX_EXTERN(__imp__sub_8284A910);
REX_EXTERN(__imp__sub_8284AB18);
REX_EXTERN(__imp__sub_8284AB88);
REX_EXTERN(__imp__sub_8284BB48);
REX_EXTERN(__imp__sub_8284B730);
REX_EXTERN(__imp__sub_82493190);
REX_EXTERN(__imp__sub_827D4BD8);
REX_EXTERN(__imp__sub_82493690);
REX_EXTERN(__imp__sub_82493240);
REX_EXTERN(__imp__sub_824931E8);
REX_EXTERN(__imp__sub_82493548);
REX_EXTERN(__imp__sub_824935E8);
REX_EXTERN(__imp__sub_82428ED8);
REX_EXTERN(__imp__sub_82428840);
REX_EXTERN(__imp__sub_828DA6F0);
REX_EXTERN(__imp__sub_828DB270);
REX_EXTERN(__imp__sub_8284C3B8);
REX_EXTERN(__imp__sub_82428878);
REX_EXTERN(__imp__sub_82429748);
REX_EXTERN(__imp__sub_82429708);
REX_EXTERN(__imp__sub_828EFC68);
REX_EXTERN(__imp__sub_824A0BC0);

namespace tr = bo2zm::jobs;

#if REX_PLATFORM_SWITCH

extern "C" uint32_t svcWaitForAddress(void* address, uint32_t arb_type, int64_t value, int64_t timeout);
extern "C" uint32_t svcSignalToAddress(void* address, uint32_t signal_type, int32_t value, int32_t count);
#endif

namespace {


constexpr uint32_t kDeviceOwner = BO2_GAME_ADDR(0x83AF72E4, 0x8372AB20);


constexpr uint32_t kReleaseInLoop1 = BO2_GAME_ADDR(0x82493630, 0x823CA838);
constexpr uint32_t kReleaseInLoop2 = BO2_GAME_ADDR(0x824935A0, 0x823CA7A8);
constexpr uint32_t kHelpInGroup1 = BO2_GAME_ADDR(0x824936EC, 0x823CA8F4);
constexpr uint32_t kHelpInGroup2 = BO2_GAME_ADDR(0x82493754, 0x823CA95C);


constexpr uint32_t kHelpServer = BO2_GAME_ADDR(0x8243C564, 0);
constexpr uint32_t kKeyServer = BO2_GAME_ADDR(0x829827E0, 0x82751250) + 16;


constexpr int kMissesSection = 3;
constexpr int kPutBacksRow = 4;

constexpr uint32_t kPutBack1 = BO2_GAME_ADDR(0x8284B20C, 0x8262E2BC);
constexpr uint32_t kPutBack2 = BO2_GAME_ADDR(0x8284B480, 0x8262E530);
constexpr uint32_t kPutBack3 = BO2_GAME_ADDR(0x8284B9C0, 0x8262EA70);


constexpr uint32_t Bit(tr::Reason m) { return 1u << m; }

struct Sleeper {
  std::condition_variable cv;
  uint32_t interests = 0;
  uint32_t key = 0;
  bool warned = false;
};

std::mutex g_mutex;
std::vector<Sleeper*> g_sleeping;
std::array<std::atomic<uint32_t>, tr::kNumWarnings> g_changes{};
std::array<std::atomic<int32_t>, tr::kNumWarnings> g_interested{};


constexpr int kSlots = 64;
constexpr uint32_t kIdle = 0;
constexpr uint32_t kSleeping = 1;
constexpr uint32_t kWarned = 2;
struct Slot {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> active{false};
  std::atomic<uint32_t> interests{0};
  std::atomic<uint32_t> key{0};
  bool warned = false;
  std::atomic<uint32_t> word{kIdle};
};
static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);

#if REX_PLATFORM_SWITCH

void WaitWord(std::atomic<uint32_t>& word, uint32_t expected, int64_t timeout_ns) {
  constexpr uint32_t kArbitrationTypeWaitIfEqual = 2;
  svcWaitForAddress(&word, kArbitrationTypeWaitIfEqual, int64_t(int32_t(expected)), timeout_ns);
}

void WakeWord(std::atomic<uint32_t>& word) {
  constexpr uint32_t kSignalTypeSignal = 0;
  svcSignalToAddress(&word, kSignalTypeSignal, 0, 1);
}
#endif
std::array<Slot, kSlots> g_slots;
std::atomic<int> g_slots_taken{0};
std::atomic<int32_t> g_shared_sleepers{0};

std::atomic<uint64_t> g_own_sleeps{0}, g_own_woken{0}, g_shared_sleeps{0}, g_shared_woken{0}, g_slot_wakes{0};
std::atomic<uint64_t> g_waits_report_ns{0};


bool Wants(uint32_t interests, uint32_t sleeper_key, tr::Reason m, uint32_t key) {

  const bool with_key = m == tr::kWarningEnded || m == tr::kWarningJob;
  return (interests & (1u << m)) && (!with_key || !sleeper_key || !key || sleeper_key == key);
}

bool OwnLocks() {
  static const bool on = REXCVAR_GET(bo2zm_waits_own_locks);
  static const bool ab = REXCVAR_GET(bo2zm_waits_own_locks_ab);
  if (ab) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & 1) != 0;
    }
  }
  return on;
}


int MySlot() {
  thread_local int slot = -2;
  if (slot == -2) {
    const int i = g_slots_taken.fetch_add(1, std::memory_order_acq_rel);
    slot = i < kSlots ? i : -1;
  }
  return slot;
}

struct Seen {
  uint32_t v[tr::kNumWarnings];
};

Seen Look() {
  Seen x;
  for (int m = 0; m < tr::kNumWarnings; ++m) {
    x.v[m] = g_changes[m].load(std::memory_order_seq_cst);
  }
  return x;
}

bool Change(const Seen& seen, uint32_t interests) {
  for (int m = 0; m < tr::kNumWarnings; ++m) {
    if ((interests & (1u << m)) && g_changes[m].load(std::memory_order_seq_cst) != seen.v[m]) {
      return true;
    }
  }
  return false;
}


void Warn(tr::Reason m, uint32_t key = 0) {
  g_changes[m].fetch_add(1, std::memory_order_seq_cst);
  if (tr::ReportActive()) {
    tr::g_warnings[m].fetch_add(1, std::memory_order_relaxed);
  }
  if (g_interested[m].load(std::memory_order_seq_cst) == 0) {
    return;
  }
  const int slots = std::min(g_slots_taken.load(std::memory_order_acquire), kSlots);
  for (int i = 0; i < slots; ++i) {
    Slot& s = g_slots[i];
#if REX_PLATFORM_SWITCH
    if (s.word.load(std::memory_order_seq_cst) != kSleeping ||
        !Wants(s.interests.load(std::memory_order_relaxed), s.key.load(std::memory_order_relaxed), m, key)) {
      continue;
    }
    uint32_t expected = kSleeping;
    if (s.word.compare_exchange_strong(expected, kWarned, std::memory_order_seq_cst)) {
      WakeWord(s.word);
      g_slot_wakes.fetch_add(1, std::memory_order_relaxed);
    }
    continue;
#endif
    if (!s.active.load(std::memory_order_seq_cst) ||
        !Wants(s.interests.load(std::memory_order_relaxed), s.key.load(std::memory_order_relaxed), m, key)) {
      continue;
    }
    bool wake = false;
    {
      std::lock_guard lock(s.mutex);
      if (s.active.load(std::memory_order_relaxed) && !s.warned) {
        s.warned = true;
        wake = true;
      }
    }
    if (wake) {
      s.cv.notify_one();
      g_slot_wakes.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (g_shared_sleepers.load(std::memory_order_seq_cst) == 0) {
    return;
  }
  std::lock_guard lock(g_mutex);
  for (Sleeper* d : g_sleeping) {
    if (Wants(d->interests, d->key, m, key)) {
      d->warned = true;
      d->cv.notify_one();
    }
  }
}

bool AnyActive() {
  static const bool active = REXCVAR_GET(bo2zm_blocking_waits);
  return active;
}

std::chrono::microseconds Limit() {
  static const auto limit = std::chrono::microseconds(REXCVAR_GET(bo2zm_waits_max_us));
  return limit;
}


std::chrono::microseconds PauseDependency() {
  static const auto pause = std::chrono::microseconds(
      50000ull * 1000000ull / std::max<uint64_t>(1, rex::chrono::Clock::guest_tick_frequency()) + 100);
  return pause;
}

uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}


void ReportWaits() {
  const uint64_t now = NowNs();
  uint64_t last = g_waits_report_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_waits_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10'000'000'000ull ||
      !g_waits_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t own = g_own_sleeps.exchange(0, std::memory_order_relaxed);
  const uint64_t own_woken = g_own_woken.exchange(0, std::memory_order_relaxed);
  const uint64_t shared = g_shared_sleeps.exchange(0, std::memory_order_relaxed);
  const uint64_t shared_woken = g_shared_woken.exchange(0, std::memory_order_relaxed);
  const uint64_t wakes = g_slot_wakes.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[waits] last {:.0f} s: {} sleeps with a lock of their own ({} woken by a change, {} slots taken), {} on "
              "the shared lock ({} woken); {} wakes given to slots",
              double(now - last) / 1e9, own, own_woken, std::min(g_slots_taken.load(), kSlots), shared,
              shared_woken, wakes);
}


bool Sleep(tr::Room room, const Seen& seen, uint32_t interests, uint32_t key,
            std::chrono::microseconds limit) {
  static const uint32_t rooms = uint32_t(REXCVAR_GET(bo2zm_wait_sites));
  if (!(rooms & (1u << room))) {
    return true;
  }
  thread_local Sleeper yo;
  const bool report = tr::ReportActive();
  const uint64_t t0 = report ? NowNs() : 0;
  bool warned;
  const int slot = OwnLocks() ? MySlot() : -1;
  if (slot >= 0) {

    Slot& s = g_slots[slot];
    s.interests.store(interests, std::memory_order_relaxed);
    s.key.store(key, std::memory_order_relaxed);
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_add(1, std::memory_order_seq_cst);
      }
    }
#if REX_PLATFORM_SWITCH
    {

      s.word.store(kSleeping, std::memory_order_seq_cst);
      const uint64_t deadline = NowNs() + uint64_t(limit.count()) * 1000ull;
      warned = false;
      for (;;) {
        if (s.word.load(std::memory_order_seq_cst) == kWarned || Change(seen, interests)) {
          warned = true;
          break;
        }
        const uint64_t now = NowNs();
        if (now >= deadline) {
          break;
        }
        WaitWord(s.word, kSleeping, int64_t(deadline - now));
      }
      s.word.store(kIdle, std::memory_order_seq_cst);
    }
#else
    {
      std::unique_lock lock(s.mutex);
      s.warned = false;
      s.active.store(true, std::memory_order_seq_cst);
      warned = s.cv.wait_for(lock, limit, [&] { return s.warned || Change(seen, interests); });
      s.active.store(false, std::memory_order_relaxed);
    }
#endif
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_sub(1, std::memory_order_seq_cst);
      }
    }
    g_own_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (warned) {
      g_own_woken.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    std::unique_lock lock(g_mutex);
    g_shared_sleepers.fetch_add(1, std::memory_order_seq_cst);
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_add(1, std::memory_order_seq_cst);
      }
    }
    yo.interests = interests;
    yo.key = key;
    yo.warned = false;
    g_sleeping.push_back(&yo);
    warned = yo.cv.wait_for(lock, limit, [&] { return yo.warned || Change(seen, interests); });
    g_sleeping.erase(std::find(g_sleeping.begin(), g_sleeping.end(), &yo));
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_sub(1, std::memory_order_seq_cst);
      }
    }
    g_shared_sleepers.fetch_sub(1, std::memory_order_seq_cst);
    g_shared_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (warned) {
      g_shared_woken.fetch_add(1, std::memory_order_relaxed);
    }
  }
  ReportWaits();
  if (report) {
    auto& h = tr::JobThread();
    const uint64_t ns = NowNs() - t0;
    h.stalls.fetch_add(1, std::memory_order_relaxed);
    h.ns_stalls.fetch_add(ns, std::memory_order_relaxed);
    auto& s = tr::g_rooms[room];
    s.times.fetch_add(1, std::memory_order_relaxed);
    s.ns.fetch_add(ns, std::memory_order_relaxed);
    if (warned) {
      h.stalls_warned.fetch_add(1, std::memory_order_relaxed);
    } else {
      s.exhausted.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return warned;
}


bool HasTakeable(const uint8_t* base, uint32_t slot) {
  bool preferred = false;
  if (tr::Read32(base, slot + tr::kSlotOwn + tr::kQueueCount)) {
    return true;
  }
  for (int k = 0; k < tr::kQueuesPerSlot; ++k) {
    const uint32_t c = tr::Read32(base, slot + tr::kSlotQueues + 4 * k);
    if (c && tr::Read32(base, c + tr::kQueueCount)) {
      return true;
    }
    preferred |= c == tr::kQueuePreferred;
  }
  return !preferred && tr::Read32(base, tr::kQueuePreferred + tr::kQueueCount);
}


bool HasTakeableHelp(const uint8_t* base) {
  return tr::Read32(base, tr::kDefaultQueue + tr::kQueueCount) ||
         tr::Read32(base, tr::kQueuePreferred + tr::kQueueCount);
}


uint32_t IdThreadGuest(const PPCContext& ctx, const uint8_t* base) {
  return tr::Read32(base, tr::Read32(base, ctx.r13.u32 + 256) + 332);
}

bool HaveDevice(const PPCContext& ctx, const uint8_t* base) {
  return tr::Read32(base, kDeviceOwner) == IdThreadGuest(ctx, base);
}


struct ThreadStatus {
  bool taken = false;
  uint32_t key = 0;
  uint32_t type_key = 0;
  uint32_t group = 0;
  bool last_empty = false;
  Seen seen_choice{};
  int loops = 0;


  bool postponed = false;
  Seen seen_postponed{};
  uint32_t interests_postponed = 0;
  uint32_t slot_worker = 0;
  int misses_section = 0;
  int putbacks_row = 0;
};
thread_local ThreadStatus t;


void WarnYesEnded() {
  if (t.taken) {
    t.taken = false;
    t.putbacks_row = 0;
    Warn(tr::kWarningEnded, t.key);
    if (t.type_key != t.key) {
      Warn(tr::kWarningEnded, t.type_key);
    }
  }
}


template <typename F>
void Measure(uint32_t who, PPCContext& ctx, uint8_t* base, F original) {
  if (!tr::ReportActive()) [[likely]] {
    original(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const uint64_t t0 = NowNs();
  original(ctx, base);
  tr::AnnotateWait(who, lr, NowNs() - t0);
}


template <typename F>
void PollBlocking(tr::Room room, PPCContext& ctx, uint8_t* base, F original, uint32_t interests) {
  const uint32_t r3 = ctx.r3.u32;
  const Seen seen = Look();
  original(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !AnyActive() || t.loops == 0 || HasTakeableHelp(base)) {
    return;
  }
  if (HaveDevice(ctx, base)) {
    t.postponed = true;
    t.seen_postponed = seen;
    t.interests_postponed = interests;
    return;
  }
  Sleep(room, seen, interests, 0, Limit());
  ctx.r3.u64 = r3;
  original(ctx, base);
}

constexpr uint32_t kInterestsHelp = Bit(tr::kWarningJob) | Bit(tr::kWarningDeferred);

}


namespace bo2zm::native {
void WarnProgressRing() {
  Warn(tr::kWarningRing);
}
}


REX_HOOK_RAW(sub_8284AED8) {
  bo2zm::benchmark::ApplyOwnPriority();
  const uint32_t slot = ctx.r3.u32, filter = ctx.r4.u32, reg_index = ctx.r5.u32, lr = uint32_t(ctx.lr);
  WarnYesEnded();
  t.seen_choice = Look();
  __imp__sub_8284AED8(ctx, base);
  const bool with_job = ctx.r3.u32 & 0xFF;
  t.last_empty = !with_job;
  bool no_dependency = false;
  if (with_job) {
    const uint32_t dependency = tr::Read32(base, reg_index + tr::kJobDependency);
    no_dependency =
        dependency && tr::Read32(base, dependency) < tr::Read32(base, reg_index + tr::kJobGoal);
    t.taken = !no_dependency;
    const uint32_t group = tr::Read32(base, reg_index + tr::kJobGroup);
    t.type_key = tr::Read32(base, reg_index + tr::kJobType) + 16;
    t.key = group ? group : t.type_key;
  }
  if (tr::ReportActive()) {
    tr::Boot(base);
    auto& h = tr::JobThread();
    h.choose.fetch_add(1, std::memory_order_relaxed);
    if (!with_job) {
      h.choose_empty[tr::ClassifyEmpty(base, slot)].fetch_add(1, std::memory_order_relaxed);
    } else if (no_dependency) {
      h.choose_dependent.fetch_add(1, std::memory_order_relaxed);
      tr::AnnotateDependency(tr::Read32(base, reg_index + tr::kJobDependency),
                            tr::Read32(base, reg_index + tr::kJobType));
    }
  }


  if (no_dependency && AnyActive()) {
    const Seen before = Look();
    const uint32_t dependency = tr::Read32(base, reg_index + tr::kJobDependency);
    if (tr::Read32(base, dependency) < tr::Read32(base, reg_index + tr::kJobGoal)) {
      Sleep(tr::kRoomDependency, before, kInterestsHelp | Bit(tr::kWarningEvent), 0, PauseDependency());
    }
  }


  if (!with_job && AnyActive() && (lr == tr::kChooseWait1 || lr == tr::kChooseWait2)) {
    const uint32_t counters = filter ? filter : tr::kInQueue;
    if (tr::Read32(base, counters) == 0 || !HasTakeable(base, slot)) {
      Sleep(tr::kRoomType, t.seen_choice, kInterestsHelp | Bit(tr::kWarningEnded), filter, Limit());
    }
  }
}


REX_HOOK_RAW(sub_827D8DD8) {
  if (tr::ReportActive()) {
    tr::JobThread().yielded.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_827D8DD8(ctx, base);
}


REX_HOOK_RAW(sub_8284A088) {
  WarnYesEnded();
  const uint32_t slot = ctx.r3.u32;
  t.slot_worker = slot;
  const bool report = tr::ReportActive();
  const uint64_t t0 = report ? NowNs() : 0;
  if (report) {
    auto& h = tr::JobThread();
    h.wait_worker.fetch_add(1, std::memory_order_relaxed);
    if (tr::Read32(base, tr::kInQueue) != 0) {
      h.wait_no_sleep.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (AnyActive()) {
    Seen seen = t.seen_choice;
    for (;;) {
      if (base[tr::kExit] || tr::Read32(base, tr::kPause) || HasTakeable(base, slot)) {
        break;
      }

      if (g_changes[tr::kWarningDeferred].load(std::memory_order_seq_cst) != t.seen_choice.v[tr::kWarningDeferred] &&
          tr::Read32(base, tr::kInQueue) != 0) {
        break;
      }


      if (!Sleep(tr::kRoomWorker, seen, kInterestsHelp, 0, 4 * Limit()) && tr::Read32(base, tr::kInQueue)) {
        break;
      }
      seen = Look();
    }
  }
  __imp__sub_8284A088(ctx, base);
  if (report) {
    tr::JobThread().ns_wait_worker.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
}


REX_HOOK_RAW(sub_8284A910) {
  const uint32_t job = ctx.r3.u32, queue = ctx.r4.u32;
  if (tr::ReportActive()) {
    tr::JobThread().queued.fetch_add(1, std::memory_order_relaxed);
    tr::AnnotateQueued(base, job, queue);
  }
  const uint32_t group = tr::Read32(base, job + tr::kJobGroup);
  const uint32_t key = group ? group : tr::Read32(base, job + tr::kJobType) + 16;
  __imp__sub_8284A910(ctx, base);
  Warn(tr::kWarningJob, key);
}


REX_HOOK_RAW(sub_8284C3B8) {
  const uint32_t lr = uint32_t(ctx.lr);
  const bool put_back = lr == kPutBack1 || lr == kPutBack2 || lr == kPutBack3;
  if (put_back && tr::ReportActive()) {
    tr::JobThread().returned.fetch_add(1, std::memory_order_relaxed);
    tr::AnnotateReturned(tr::Read32(base, ctx.r4.u32 + tr::kJobType));
  }
  __imp__sub_8284C3B8(ctx, base);
  if (!put_back) {
    return;
  }
  t.taken = false;
  const bool worker = t.slot_worker != 0;
  if (!AnyActive() || (!worker && t.loops == 0) || ++t.putbacks_row < kPutBacksRow) {
    return;
  }
  t.putbacks_row = 0;
  if (!worker && HaveDevice(ctx, base)) {
    return;
  }
  uint32_t interests = kInterestsHelp | Bit(tr::kWarningEnded) | Bit(tr::kWarningLock);
  if (!worker) {
    interests |= Bit(tr::kWarningEvent) | Bit(tr::kWarningRing);
  }
  Sleep(tr::kRoomPutBack, t.seen_choice, interests, 0, Limit());
}


bool CullWait() {
  static const int factor = REXCVAR_GET(bo2zm_waits_cull_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  static const bool on = REXCVAR_GET(bo2zm_waits_cull);
  return on;
}

REX_EXTERN(__imp__sub_828DED00);
REX_HOOK_RAW(sub_828DED00) {
  constexpr uint32_t kCullPending = BO2_GAME_ADDR(0x82A99A30, 0x8285D640);
  const auto pending = [&] { return (tr::Read32(base, kCullPending) | tr::Read32(base, kCullPending + 4)) != 0; };
  if (CullWait() && AnyActive() && pending() && !HaveDevice(ctx, base)) {
    const Seen seen = Look();
    if (pending()) {
      Sleep(tr::kRoomCull, seen, Bit(tr::kWarningEnded), kCullPending, Limit());
    }
  }
  __imp__sub_828DED00(ctx, base);
}


REX_HOOK_RAW(sub_8284AB18) {
  __imp__sub_8284AB18(ctx, base);
  Warn(tr::kWarningDeferred);
}


REX_HOOK_RAW(sub_8284AB88) {
  __imp__sub_8284AB88(ctx, base);
  Warn(tr::kWarningJob);
}


REX_HOOK_RAW(sub_8284BB48) {
  const uint32_t lr = uint32_t(ctx.lr);
  const bool report = tr::ReportActive();
  const uint64_t t0 = report ? NowNs() : 0;
  __imp__sub_8284BB48(ctx, base);
  WarnYesEnded();
  if (report) {
    auto& h = tr::JobThread();
    h.helps.fetch_add(1, std::memory_order_relaxed);
    h.ns_helps.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
  if (AnyActive() && t.last_empty && (lr == kHelpInGroup1 || lr == kHelpInGroup2) && !HasTakeableHelp(base)) {
    Sleep(tr::kRoomGroup, t.seen_choice, kInterestsHelp | Bit(tr::kWarningEnded), t.group, Limit());
  }
}


REX_HOOK_RAW(sub_8284B730) {
  const bool report = tr::ReportActive();
  const uint32_t lr = uint32_t(ctx.lr);
  const uint64_t t0 = report ? NowNs() : 0;
  __imp__sub_8284B730(ctx, base);
  WarnYesEnded();
  if (report) {
    auto& h = tr::JobThread();
    const uint64_t ns = NowNs() - t0;
    h.waits_type.fetch_add(1, std::memory_order_relaxed);
    h.ns_waits_type.fetch_add(ns, std::memory_order_relaxed);
    tr::AnnotateWait(0x8284B730, lr, ns);
  }
}


REX_HOOK_RAW(sub_824935E8) {
  ++t.loops;
  Measure(0x824935E8, ctx, base, __imp__sub_824935E8);
  --t.loops;
  t.postponed = false;
}
REX_HOOK_RAW(sub_82493548) {
  ++t.loops;
  Measure(0x82493548, ctx, base, __imp__sub_82493548);
  --t.loops;
  t.postponed = false;
}


REX_HOOK_RAW(sub_82493190) { Measure(0x82493190, ctx, base, __imp__sub_82493190); }


REX_HOOK_RAW(sub_827D4BD8) { Measure(0x827D4BD8, ctx, base, __imp__sub_827D4BD8); }

REX_HOOK_RAW(sub_82493690) {
  const uint32_t earlier = t.group;
  t.group = ctx.r3.u32;
  Measure(0x82493690, ctx, base, __imp__sub_82493690);
  t.group = earlier;
}
REX_HOOK_RAW(sub_82493240) { Measure(0x82493240, ctx, base, __imp__sub_82493240); }


REX_HOOK_RAW(sub_824931E8) {
  const uint32_t lr = uint32_t(ctx.lr);
  Measure(0x824931E8, ctx, base, __imp__sub_824931E8);
  if (AnyActive() && lr == kHelpServer && t.last_empty && !HasTakeableHelp(base)) {
    Sleep(tr::kRoomServer, t.seen_choice, kInterestsHelp | Bit(tr::kWarningEnded), kKeyServer,
           Limit());
  }
}


REX_HOOK_RAW(sub_82428ED8) {
  bo2zm::benchmark::ApplyOwnPriority();
  const bool poll = ctx.r3.u32 == 0;
#if REX_PLATFORM_SWITCH


  if (poll && !AnyActive()) {
    thread_local uint32_t idle = 0;
    if (tr::Read32(base, tr::kInQueue) != 0) {
      idle = 0;
    } else if (++idle > 32) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
#endif
  if (poll) {
    PollBlocking(tr::kRoomPollFrame, ctx, base, __imp__sub_82428ED8,
                     kInterestsHelp | Bit(tr::kWarningEvent));
  } else {
    __imp__sub_82428ED8(ctx, base);
  }
  if (tr::ReportActive() && poll) {
    auto& h = tr::JobThread();
    h.polls.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32 & 0xFF) {
      h.polls_ready.fetch_add(1, std::memory_order_relaxed);
    }
  }
}


REX_HOOK_RAW(sub_82428840) {
  PollBlocking(tr::kRoomPollDone, ctx, base, __imp__sub_82428840, kInterestsHelp | Bit(tr::kWarningEvent));
}


REX_HOOK_RAW(sub_828DA6F0) {
  if (bo2zm::native::IsActive()) {
    PollBlocking(tr::kRoomPollRing, ctx, base, __imp__sub_828DA6F0,
                     kInterestsHelp | Bit(tr::kWarningRing));
  } else {
    __imp__sub_828DA6F0(ctx, base);
  }
}


REX_HOOK_RAW(sub_828DB270) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_828DB270(ctx, base);
  if (t.postponed && (lr == kReleaseInLoop1 || lr == kReleaseInLoop2)) {
    t.postponed = false;
    if (!HasTakeableHelp(base)) {
      Sleep(tr::kRoomAfterRelease, t.seen_postponed, t.interests_postponed, 0, Limit());
    }
  }
}


constexpr uint32_t kSectionModels = 6;
REX_HOOK_RAW(sub_824A0BC0) {
  const uint32_t index = ctx.r3.u32;
  __imp__sub_824A0BC0(ctx, base);
  if (index == kSectionModels) {
    Warn(tr::kWarningLock);
  }
}


REX_HOOK_RAW(sub_828EFC68) {
  const Seen seen = Look();
  __imp__sub_828EFC68(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    t.misses_section = 0;
    return;
  }
  if (!AnyActive() || t.slot_worker == 0 || ++t.misses_section < kMissesSection) {
    return;
  }
  t.misses_section = 0;
  Sleep(tr::kRoomLock, seen, Bit(tr::kWarningLock) | kInterestsHelp, 0, Limit());
  __imp__sub_828EFC68(ctx, base);
}

REX_HOOK_RAW(sub_82428878) {
  __imp__sub_82428878(ctx, base);
  Warn(tr::kWarningEvent);
}
REX_HOOK_RAW(sub_82429748) {
  __imp__sub_82429748(ctx, base);
  Warn(tr::kWarningEvent);
}
REX_HOOK_RAW(sub_82429708) {
  __imp__sub_82429708(ctx, base);
  Warn(tr::kWarningEvent);
}


REXCVAR_DEFINE_BOOL(bo2zm_sound_without_queue, true, "BO2",
                    "The sound mixer runs its own mix jobs (sd_mix_voice, sd_mix_master, ...) while it waits for "
                    "them, instead of waiting for a job worker to take them. false = the game's own wait");

REX_EXTERN(__imp__sub_82442B90);
REX_EXTERN(__imp__sub_8243F3C8);
REX_EXTERN(__imp__sub_8243F538);
REX_EXTERN(__imp__sub_8258A368);
REX_EXTERN(__imp__sub_8258A4C0);
REX_EXTERN(__imp__sub_82428468);
REX_EXTERN(sub_82493190);

namespace {


thread_local uint32_t t_step_sound = 0;


std::atomic<uint32_t> g_steps_sound{0};
std::atomic<uint64_t> g_ns_sound{0};
std::atomic<uint64_t> g_max_ns_sound{0};
std::atomic<uint64_t> g_next_summary_sound{0};

template <typename F>
void SoundStep(PPCContext& ctx, uint8_t* base, uint32_t definition, F original) {
  const uint32_t earlier = t_step_sound;
  t_step_sound = REXCVAR_GET(bo2zm_sound_without_queue) ? definition : 0;
  original(ctx, base);
  t_step_sound = earlier;
}

void SummarizeSound(uint64_t now) {
  uint64_t next = g_next_summary_sound.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_summary_sound.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next ||
      !g_next_summary_sound.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  const uint32_t steps = g_steps_sound.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_ns_sound.exchange(0, std::memory_order_relaxed);
  const uint64_t max_ns = g_max_ns_sound.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[sound] 10 s: {} mix steps run by the mixer itself, {:.1f} ms in total, longest {:.2f} ms", steps,
              double(ns) / 1e6, double(max_ns) / 1e6);
}

}

REX_HOOK_RAW(sub_82442B90) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982C70, 0x82746E70), __imp__sub_82442B90); }
REX_HOOK_RAW(sub_8243F3C8) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982B70, 0x82746D70), __imp__sub_8243F3C8); }
REX_HOOK_RAW(sub_8243F538) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982BF0, 0x82746DF0), __imp__sub_8243F538); }
REX_HOOK_RAW(sub_8258A368) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82A7D1D0, 0x82848F50), __imp__sub_8258A368); }
REX_HOOK_RAW(sub_8258A4C0) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82A7D1F0, 0x82848F70), __imp__sub_8258A4C0); }


REX_HOOK_RAW(sub_82428468) {
  if (const uint32_t definition = t_step_sound) [[unlikely]] {
    t_step_sound = 0;
    const uint64_t event = ctx.r3.u64;
    const uint64_t lr = ctx.lr;
    const uint64_t t0 = NowNs();
    ctx.r3.u64 = definition;
    sub_82493190(ctx, base);
    ctx.r3.u64 = event;
    ctx.lr = lr;
    const uint64_t now = NowNs();
    const uint64_t ns = now - t0;
    g_steps_sound.fetch_add(1, std::memory_order_relaxed);
    g_ns_sound.fetch_add(ns, std::memory_order_relaxed);
    uint64_t max_ns = g_max_ns_sound.load(std::memory_order_relaxed);
    while (ns > max_ns && !g_max_ns_sound.compare_exchange_weak(max_ns, ns, std::memory_order_relaxed)) {
    }
    SummarizeSound(now);
  }
  __imp__sub_82428468(ctx, base);
}
