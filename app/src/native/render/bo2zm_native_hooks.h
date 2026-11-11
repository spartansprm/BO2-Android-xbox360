#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace bo2zm::native {

class NativeShaders;
struct ShaderInput;

enum class DrawFunction : uint8_t {
  kVertices,
  kIndexed,
  kVerticesUP,
  kIndexedUP,
};

struct DrawRecord {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint32_t args[4] = {};
  DrawFunction function = DrawFunction::kVertices;
  uint16_t vegetation = 0;
  uint64_t shadow = 0;
};


constexpr uint32_t kGroupsMirror = 9;
struct MirrorSnapshot {
  uint64_t mask[kGroupsMirror] = {};
  uint32_t base_register[kGroupsMirror] = {};
  uint32_t state[kGroupsMirror][64] = {};
  uint32_t fetch[192] = {};
  uint32_t constants[2048] = {};


  uint32_t constants_mirror[2048] = {};
  uint8_t constants_from[2048] = {};
};


enum : uint8_t { kFromMirror = 0, kFromLiteral = 1, kFromGame = 2, kFromBuffer = 3, kFromUnknown = 4 };

void LearnGroupMirror(uint32_t register_base, uint64_t mask, uint32_t displacement);

bool ReadSnapshot(uint64_t sequence, MirrorSnapshot& output);


constexpr uint32_t kMarkerMagic = 0x425A4D00;
constexpr uint32_t kMarkerApply = 1;
constexpr uint32_t kMarkerCheck = 2;


inline bool DumpSpan(uint32_t reg_index, uint32_t count) {
  static constexpr uint8_t kSize2000[8] = {16, 0, 21, 5, 12, 21, 38, 8};
  if (count == 0) {
    return false;
  }
  if (reg_index >= 0x2000 && reg_index < 0x2400) {
    return (reg_index & 0x7Fu) + count <= kSize2000[(reg_index - 0x2000) >> 7];
  }
  if (reg_index >= 0x4000 && reg_index < 0x4800) {
    return reg_index + count <= (reg_index < 0x4400 ? 0x4400u : 0x4800u);
  }
  if (reg_index >= 0x4800 && reg_index < 0x48C0) {
    return reg_index + count <= 0x48C0;
  }
  if (reg_index >= 0x4900 && reg_index < 0x4928) {
    return reg_index + count <= 0x4928;
  }
  return false;
}


void ActivateConsumerMarkers(bool active);


inline std::atomic<uint32_t> g_publications_ring{0};


void AnnotateCheckMarker(bool equal, uint32_t sequence, uint32_t reg_index, uint32_t in_registers,
                                uint32_t in_marker);


constexpr uint32_t kDrawApply = 2;
constexpr uint32_t kDrawCheck = 3;
constexpr uint32_t kWordsDraw = 9;

bool TakeDrawInOngoing(DrawRecord& reg_index);

uint32_t DecideModeDraw();

void DeliverDraw(const DrawRecord& reg_index, uint32_t mode, bool in_marker);

void FinishDraw();


bool RecordForStream(DrawFunction function, const uint8_t* base, uint32_t device, uint32_t r4, uint32_t r5,
                     uint32_t r6, uint32_t r7, DrawRecord& record, uint32_t& mode);


void NoteGameConstants(const uint8_t* base, uint32_t device, uint32_t pointer, uint32_t vectors);


void BeforeFlush(const uint8_t* base, uint32_t device);


void BeginBuffer(uint32_t device, uint32_t buffer);
void EndBuffer(uint32_t device);
void AfterRun(uint32_t device, uint32_t buffer);


void AfterDraw(const uint8_t* base);

std::string StreamConstantsReport();

bool D3DShadowOn();

void StreamRecordCounts(uint64_t& records, uint64_t& checking);

void BeforeDraw(uint8_t* base, uint32_t device, DrawFunction function, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7);


void AnnotateCheckDraw(bool equal, uint32_t kind, const DrawRecord* lookup, const DrawRecord* marker);


constexpr uint16_t kVegHas = 1u << 0;
constexpr uint16_t kVegYes = 1u << 1;
constexpr uint16_t kVegOcclusion = 1u << 2;
constexpr uint16_t kVegSettings = 1u << 3;
constexpr uint16_t kVegBlock = 1u << 4;
constexpr uint16_t kVegTargets = 1u << 5;
constexpr uint16_t kVegWouldSkip = 1u << 6;
constexpr uint16_t kVegSample = 1u << 7;
constexpr uint32_t kVegReason = 8;
constexpr uint16_t kVegReasonNoShaders = 1;
constexpr uint16_t kVegReasonUnknown = 2;
constexpr uint16_t kVegReasonMode = 3;
constexpr uint16_t kVegReasonColor = 4;
constexpr uint16_t kVegReasonNoDiscard = 5;


struct VegetationDetail {
  uint32_t mode = 0;
  uint32_t mask = 0;
  uint32_t control = 0;
  int32_t vs = -1;
  int32_t ps = -1;
  uint32_t outputs = 0;
  bool discards = false;
  bool structural = false;
  bool settings = false;
  bool occlusion = false;
  bool early = false;
};


uint16_t DecideVegetation(DrawFunction function, uint8_t* base, uint32_t vulkan_device, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip);

void AnnotateQueryD3D(const uint8_t* base, uint32_t query, uint32_t flags);

uint16_t IdentityForVegetation(uint16_t flags, bool with_shaders);

void AnnotateVegetationRing(uint16_t flags, const VegetationDetail& ring);

void AnnotateVegetationModel(uint16_t flags, const VegetationDetail& ring);

struct HookStats {
  uint64_t created_vs = 0;
  uint64_t known_vs = 0;
  uint64_t created_ps = 0;
  uint64_t known_ps = 0;
  uint64_t lost = 0;
};


void ActivateHooks(NativeShaders* shaders);


void RegisterShaderObject(const uint8_t* base, uint32_t object, uint32_t physical, bool vertices);


const NativeShaders* LibraryActive();


void AnnotateDraw(DrawFunction function, const uint8_t* base, uint32_t vulkan_device, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation = 0);


bool ExtractDraw(DrawRecord& reg_index);
const ShaderInput* ObjectShader(uint32_t object);
uint64_t GenerationObjects();
HookStats HooksStats();

}
