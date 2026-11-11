#pragma once
// Join codes for internet play: a short code that carries the host's public IPv4 address, so two players can find each
// other without a server of ours. The host reads its code off the screen and the other players type it in.
//
// A code is 40 bits written as 8 characters of Crockford's base 32 (0-9 and the letters without I, L, O and U), shown
// in two groups of four, such as "8F3K-Q7M2":
//   bits 39-8  the IPv4 address, most significant byte first
//   bits 7-0   a CRC-8 (polynomial 0x1D) of the four address bytes, started from a value that depends on kVersion
// The format has no version field: a future format changes kVersion, and with it every check byte, so an old code reads
// as a typo instead of sending the player to a wrong address.
//
// Reading one ignores case, dashes and spaces, and takes O for 0 and I or L for 1, as Crockford's scheme intends.
// x^8 + x^4 + x^3 + x^2 + 1 is irreducible, so the check catches every error burst up to 8 bits long: any single
// mistyped character (5 bits) and any two neighbouring characters swapped (the same 5-bit difference in two places,
// e * (x^5 + 1), which the polynomial cannot divide). Other typos slip through one time in 256.
//
// Header-only and free of the runtime's other headers, so the codec test builds it on its own.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rex::system::join_code {

constexpr uint32_t kVersion = 2;  // 1 was the 10-character format
constexpr size_t kLength = 8;     // characters, without the dash
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

enum class Result { kOk, kLength, kCharacter, kChecksum };

// CRC-8, polynomial 0x1D, most significant bit first.
inline uint8_t Crc8(const uint8_t* data, size_t size, uint8_t crc) {
  for (size_t i = 0; i < size; ++i) {
    crc = uint8_t(crc ^ data[i]);
    for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x1D) : uint8_t(crc << 1);
  }
  return crc;
}

inline uint8_t Check(uint32_t ip, uint32_t version = kVersion) {
  const uint8_t salt[4] = {'B', 'Z', 'N', uint8_t(version)};
  const uint8_t bytes[4] = {uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
  return Crc8(bytes, sizeof(bytes), Crc8(salt, sizeof(salt), 0xFF));
}

// The code for this address (host byte order) without its dash: "XXXXXXXX". The relay's topics are made from it.
inline std::string Plain(uint32_t ip, uint32_t version = kVersion) {
  const uint64_t value = (uint64_t(ip) << 8) | Check(ip, version);
  std::string code;
  for (size_t i = 0; i < kLength; ++i) code += kAlphabet[(value >> (5 * (kLength - 1 - i))) & 31];
  return code;
}

// The code for this address (host byte order), as shown: "XXXX-XXXX".
inline std::string Make(uint32_t ip, uint32_t version = kVersion) {
  const std::string plain = Plain(ip, version);
  return plain.substr(0, kLength / 2) + "-" + plain.substr(kLength / 2);
}

// The value of one character, or -1 for a character no code has.
inline int Digit(char c) {
  if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  if (c == 'O') return 0;
  if (c == 'I' || c == 'L') return 1;
  for (int i = 0; i < 32; ++i) {
    if (kAlphabet[i] == c) return i;
  }
  return -1;
}

// Reads a code as typed. On kOk, *ip is the address (host byte order).
inline Result Read(std::string_view text, uint32_t* ip) {
  uint64_t value = 0;
  size_t digits = 0;
  for (const char c : text) {
    if (c == '-' || c == ' ' || c == '\t') continue;
    const int digit = Digit(c);
    if (digit < 0) return Result::kCharacter;
    if (++digits > kLength) return Result::kLength;
    value = (value << 5) | uint64_t(digit);
  }
  if (digits != kLength) return Result::kLength;
  const uint32_t address = uint32_t(value >> 8);
  if (Check(address) != uint8_t(value)) return Result::kChecksum;
  if (ip) *ip = address;
  return Result::kOk;
}

// One line for the player.
inline const char* Describe(Result result) {
  switch (result) {
    case Result::kOk: return "The code is valid.";
    case Result::kLength: return "A join code has 8 letters and digits.";
    case Result::kCharacter: return "A join code has only letters and digits (no U).";
    case Result::kChecksum:
      return "That code has a typo, or comes from another version of the game: check each character.";
  }
  return "The code is not valid.";
}

}  // namespace rex::system::join_code
