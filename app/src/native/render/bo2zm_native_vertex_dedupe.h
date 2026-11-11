#pragma once

#include <array>
#include <cstdint>

namespace bo2zm::native {


class DedupeVertices {
 public:


  static constexpr size_t kSlots = 16384;


  void NewFrame(uint64_t frame) {

    frame_ = frame == kEmpty ? 0 : frame;
  }


  void Forget() {
    ++generation_;
    ++forgets_;
  }


  bool Find(uint64_t address, uint32_t bytes, uint32_t order, uint64_t& offset,
              uint64_t hash = 0) {
    ++queries_;
    const uint64_t key = Key(address, order);

    const size_t index = Index(key, bytes);
    const Input* found = nullptr;
    for (size_t way = 0; way < 2 && !found; ++way) {
      const Input& e = table_[index ^ way];
      if (e.frame == frame_ && e.generation == generation_ && e.key == key && e.bytes == bytes) {
        found = &e;
      }
    }
    if (!found) {
      return false;
    }
    const Input& e = *found;
    if (hash != 0 && e.hash != hash) {
      ++discrepancies_;
      return false;
    }
    ++hits_;
    bytes_saved_ += bytes;
    offset = e.offset;
    return true;
  }


  void Annotate(uint64_t address, uint32_t bytes, uint32_t order, uint64_t offset,
              uint64_t hash = 0) {
    const uint64_t key = Key(address, order);


    const size_t index = Index(key, bytes);
    const auto live = [&](const Input& c) { return c.frame == frame_ && c.generation == generation_; };
    const auto same = [&](const Input& c) { return c.key == key && c.bytes == bytes; };
    size_t pick = index;
    if (live(table_[index]) && !same(table_[index])) {
      pick = index ^ 1;
    }
    Input& e = table_[pick];
    if (live(e) && !same(e)) {
      ++collisions_;
    }
    e.key = key;
    e.bytes = bytes;
    e.offset = offset;
    e.hash = hash;
    e.frame = frame_;
    e.generation = generation_;
  }


  uint64_t queries() const { return queries_; }
  uint64_t hits() const { return hits_; }
  uint64_t bytes_saved() const { return bytes_saved_; }
  uint64_t collisions() const { return collisions_; }
  uint64_t forgets() const { return forgets_; }
  uint64_t discrepancies() const { return discrepancies_; }

 private:
  static constexpr uint64_t kEmpty = ~uint64_t(0);

  struct Input {
    uint64_t key = 0;
    uint64_t offset = 0;
    uint64_t hash = 0;
    uint64_t frame = kEmpty;
    uint32_t bytes = 0;
    uint32_t generation = 0;
  };
  static_assert(sizeof(Input) == 40, "the entry has to fit in 40 bytes");

  static uint64_t Key(uint64_t address, uint32_t order) {
    return (address << 2) | (uint64_t(order) & 0x3);
  }


  static size_t Index(uint64_t key, uint32_t bytes) {
    uint64_t x = key ^ (uint64_t(bytes) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kSlots - 1);
  }

  std::array<Input, kSlots> table_{};
  uint64_t frame_ = 0;
  uint32_t generation_ = 0;

  uint64_t queries_ = 0;
  uint64_t hits_ = 0;
  uint64_t bytes_saved_ = 0;
  uint64_t collisions_ = 0;
  uint64_t forgets_ = 0;
  uint64_t discrepancies_ = 0;
};


class DedupeIndices {
 public:
  static constexpr size_t kSlots = 8192;

  struct Entry {
    uint64_t key = 0;
    uint64_t offset = 0;
    uint64_t frame = ~uint64_t(0);
    uint64_t hash = 0;
    uint32_t count = 0;
    uint32_t generation = 0;
    uint16_t min = 0;
    uint16_t max = 0;
  };

  void NewFrame(uint64_t frame) { frame_ = frame == ~uint64_t(0) ? 0 : frame; }
  void Forget() { ++generation_; }


  const Entry* Find(uint32_t address, uint32_t count, uint32_t order) const {
    const Entry& e = table_[Index(Key(address, order), count)];
    if (e.frame != frame_ || e.generation != generation_ || e.key != Key(address, order) || e.count != count) {
      return nullptr;
    }
    return &e;
  }

  void Annotate(uint32_t address, uint32_t count, uint32_t order, uint16_t min, uint16_t max, uint64_t offset,
                uint64_t hash) {
    Entry& e = table_[Index(Key(address, order), count)];
    e.hash = hash;
    e.key = Key(address, order);
    e.count = count;
    e.min = min;
    e.max = max;
    e.offset = offset;
    e.frame = frame_;
    e.generation = generation_;
  }

  uint64_t frame() const { return frame_; }

 private:
  static uint64_t Key(uint32_t address, uint32_t order) { return (uint64_t(address) << 2) | (order & 0x3); }
  static size_t Index(uint64_t key, uint32_t count) {
    uint64_t x = key ^ (uint64_t(count) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kSlots - 1);
  }

  std::array<Entry, kSlots> table_{};
  uint64_t frame_ = 0;
  uint32_t generation_ = 0;
};

}
