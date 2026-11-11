#pragma once

#include "bo2zm_game.h"
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/platform.h>

namespace bo2zm::jobs {


constexpr uint32_t kStats = BO2_GAME_ADDR(0x82A97D90, 0x8285B9A0);
constexpr uint32_t kInQueue = kStats + 64;
constexpr uint32_t kInRunning = kStats + 68;
constexpr uint32_t kMaskThreads = BO2_GAME_ADDR(0x82A97D88, 0x8285B998);
constexpr uint32_t kNumSlots = BO2_GAME_ADDR(0x8479D0A0, 0x83FCC220);
constexpr uint32_t kSlots = BO2_GAME_ADDR(0x8479D0A4, 0x83FCC224);
constexpr uint32_t kSlotsHelp = BO2_GAME_ADDR(0x8479D0A8, 0x83FCC228);
constexpr uint32_t kHelpsInOngoing = BO2_GAME_ADDR(0x8479D0AC, 0x83FCC22C);
constexpr uint32_t kPause = BO2_GAME_ADDR(0x8479D0B4, 0x83FCC234);
constexpr uint32_t kWorkersWaiting = BO2_GAME_ADDR(0x8479D0B8, 0x83FCC238);
constexpr uint32_t kEventJobs = BO2_GAME_ADDR(0x8479D0CC, 0x83FCC24C);
constexpr uint32_t kExit = BO2_GAME_ADDR(0x8479D0D0, 0x83FCC250);
constexpr uint32_t kDefaultQueue = BO2_GAME_ADDR(0x8479D0E0, 0x83FCC260);
constexpr uint32_t kListDeferred = BO2_GAME_ADDR(0x8479D1E0, 0x83FCC360);
constexpr uint32_t kQueuePreferred = BO2_GAME_ADDR(0x8479D208, 0x83FCC388);
constexpr uint32_t kEventFrame = BO2_GAME_ADDR(0x83AF71DC, 0x8372AA14);


constexpr uint32_t kQueueCount = 48;
constexpr uint32_t kQueueMask = 52;
constexpr uint32_t kQueueTarget = 56;
constexpr uint32_t kQueueOpen = 60;
constexpr uint32_t kQueuePending = 64;


constexpr uint32_t kSzSlot = 128;
constexpr uint32_t kSlotMask = 20;
constexpr uint32_t kSlotIndex = 24;
constexpr uint32_t kSlotRegistered = 28;
constexpr uint32_t kSlotOwn = 32;
constexpr uint32_t kSlotQueues = 88;
constexpr int kQueuesPerSlot = 8;


constexpr uint32_t kJobType = 12;
constexpr uint32_t kJobGroup = 16;


constexpr uint32_t kJobDependency = 20;
constexpr uint32_t kJobGoal = 24;


constexpr uint32_t kChooseWorker1 = BO2_GAME_ADDR(0x8284B054, 0x8262E104);
constexpr uint32_t kChooseWorker2 = BO2_GAME_ADDR(0x8284B2B8, 0x8262E368);
constexpr uint32_t kChooseHelp1 = BO2_GAME_ADDR(0x8284B384, 0x8262E434);
constexpr uint32_t kChooseHelp2 = BO2_GAME_ADDR(0x8284B548, 0x8262E5F8);
constexpr uint32_t kChooseWait1 = BO2_GAME_ADDR(0x8284B80C, 0x8262E8BC);
constexpr uint32_t kChooseWait2 = BO2_GAME_ADDR(0x8284BA64, 0x8262EB14);


inline uint32_t Read32(const uint8_t* base, uint32_t dir) {
#if REX_PLATFORM_WIN32
  dir += dir >= 0xE0000000u ? 0x1000u : 0u;
#endif
  uint32_t v;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}


struct PerThread {
  const char* label = "?";
  uint32_t id = 0;
  std::atomic<uint64_t> choose{0};
  std::atomic<uint64_t> choose_empty[3] = {};
  std::atomic<uint64_t> choose_dependent{0};
  std::atomic<uint64_t> yielded{0};
  std::atomic<uint64_t> wait_worker{0};
  std::atomic<uint64_t> wait_no_sleep{0};
  std::atomic<uint64_t> ns_wait_worker{0};
  std::atomic<uint64_t> polls{0};
  std::atomic<uint64_t> polls_ready{0};
  std::atomic<uint64_t> queued{0};
  std::atomic<uint64_t> returned{0};
  std::atomic<uint64_t> helps{0};
  std::atomic<uint64_t> ns_helps{0};
  std::atomic<uint64_t> waits_type{0};
  std::atomic<uint64_t> ns_waits_type{0};
  std::atomic<uint64_t> stalls{0};
  std::atomic<uint64_t> stalls_warned{0};
  std::atomic<uint64_t> ns_stalls{0};
};


enum Reason : int {
  kWarningJob,
  kWarningDeferred,
  kWarningEnded,
  kWarningEvent,
  kWarningRing,
  kWarningLock,
  kNumWarnings
};
inline std::atomic<uint64_t> g_warnings[kNumWarnings] = {};


enum Room : int {
  kRoomPollFrame, kRoomPollDone, kRoomPollRing, kRoomAfterRelease, kRoomWorker, kRoomType,
  kRoomGroup, kRoomServer, kRoomDependency, kRoomLock, kRoomPutBack, kRoomCull, kNumRooms
};
struct PerSite {
  std::atomic<uint64_t> times{0}, exhausted{0}, ns{0};
};
inline PerSite g_rooms[kNumRooms];


bool ReportActive();

PerThread& JobThread();

void Boot(uint8_t* base);


int ClassifyEmpty(const uint8_t* base, uint32_t slot);
void AnnotateQueued(const uint8_t* base, uint32_t job, uint32_t queue);


void AnnotateReturned(uint32_t type);

void AnnotateDependency(uint32_t counter, uint32_t type);

void AnnotateWait(uint32_t who, uint32_t lr, uint64_t ns);

}
