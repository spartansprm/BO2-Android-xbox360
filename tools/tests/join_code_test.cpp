#include <rex/system/join_code.h>

#include <cassert>
#include <cstdio>
#include <random>
#include <string>

namespace jc = rex::system::join_code;

int main() {
  const uint32_t samples[] = {0x00000000u, 0xFFFFFFFFu, 0x5DB8D822u                    , 0x08080808u, 0xC0A80105u,
                              0x7F000001u, 0x01020304u, 0xCB007107u};
  for (const uint32_t ip : samples) {
    const std::string code = jc::Make(ip);
    assert(code.size() == jc::kLength + 1 && code[4] == '-');
    assert(jc::Plain(ip) == code.substr(0, 4) + code.substr(5));
    uint32_t back = 0;
    assert(jc::Read(code, &back) == jc::Result::kOk && back == ip);
  }

  std::mt19937 rng(12345);
  long single_typos = 0, swaps = 0;
  for (int n = 0; n < 20000; ++n) {
    const uint32_t ip = rng();
    const std::string code = jc::Make(ip);
    uint32_t back = 0;
    assert(jc::Read(code, &back) == jc::Result::kOk && back == ip);


    std::string typed;
    for (const char c : code) {
      if (c == '-') continue;
      char t = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      if (c == '0' && (n & 1)) t = 'o';
      if (c == '1') t = (n & 2) ? 'i' : 'L';
      typed += t;
      if (typed.size() == 3) typed += ' ';
    }
    back = 0;
    assert(jc::Read(typed, &back) == jc::Result::kOk && back == ip);


    const std::string plain = jc::Plain(ip);
    for (size_t at = 0; at < plain.size(); ++at) {
      for (int d = 0; d < 32; ++d) {
        if (jc::kAlphabet[d] == plain[at]) continue;
        std::string wrong = plain;
        wrong[at] = jc::kAlphabet[d];
        assert(jc::Read(wrong, &back) == jc::Result::kChecksum);
        ++single_typos;
      }
    }

    for (size_t i = 0; i + 1 < plain.size(); ++i) {
      if (plain[i] == plain[i + 1]) continue;
      std::string swapped = plain;
      std::swap(swapped[i], swapped[i + 1]);
      assert(jc::Read(swapped, &back) == jc::Result::kChecksum);
      ++swaps;
    }
  }

  uint32_t ip = 0;
  assert(jc::Read("", &ip) == jc::Result::kLength);
  assert(jc::Read("ABCD-EFG", &ip) == jc::Result::kLength);
  assert(jc::Read("ABCD-EFGHJ", &ip) == jc::Result::kLength);
  assert(jc::Read("ABCDE-FGHJK", &ip) == jc::Result::kLength);
  assert(jc::Read("ABCD-EFGU", &ip) == jc::Result::kCharacter);
  assert(jc::Read("ABCD-EF!H", &ip) == jc::Result::kCharacter);


  int other_ok = 0;
  for (int n = 0; n < 4096; ++n) other_ok += jc::Read(jc::Make(rng(), 1), &ip) == jc::Result::kOk;
  assert(other_ok == 0);
  for (const jc::Result r : {jc::Result::kOk, jc::Result::kLength, jc::Result::kCharacter, jc::Result::kChecksum}) {
    assert(jc::Describe(r) && *jc::Describe(r));
  }
  std::printf("join codes: round trips, typing rules, %ld single typos and %ld swaps caught, %d of 4096 codes of the "
              "previous version accepted; 93.184.216.34 is %s\n",
              single_typos, swaps, other_ok, jc::Make(0x5DB8D822u).c_str());
  return 0;
}
