// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <cstddef>
#include <span>

namespace bo2zm::native::shader_probe {
inline constexpr uint32_t kFlat = uint32_t(1) << 30;
inline constexpr uint64_t kTarget = 0x7e08c83e9feb2809ull;
inline bool Target(uint64_t hash,uint32_t ordinal,uint32_t width,uint32_t height,
                   std::span<const uint32_t> formats) {

  return hash==kTarget && ordinal==3 && width==880 && (height==768 || height==720) && formats.size()==5 &&
      formats[0]==37 && !formats[1] && !formats[2] && !formats[3] && formats[4]==129;
}
enum class Reason : uint32_t { kEligible, kNoColorShader, kShaderDepth, kDiscard,
    kAlphaTest, kBlend, kQuery, kUnsafeShader, kCount };
inline Reason Eligible(bool shader,uint32_t outputs,bool discards,bool alpha,bool blend,
                       bool query,bool safe) {
  if(!shader || (outputs&0xf)!=1)return Reason::kNoColorShader;
  if(outputs&0x10)return Reason::kShaderDepth;
  if(discards)return Reason::kDiscard;
  if(alpha)return Reason::kAlphaTest;
  if(blend)return Reason::kBlend;
  if(query)return Reason::kQuery;
  if(!safe)return Reason::kUnsafeShader;
  return Reason::kEligible;
}


inline bool CoverageSafe(std::span<const uint32_t> w) {
  if(w.size()<5 || w[0]!=0x07230203)return false;
  for(std::size_t i=5;i<w.size();) {
    const uint32_t n=w[i]>>16,op=w[i]&0xffff;
    if(!n || n>w.size()-i)return false;
    if(op==71 || op==72) {
      const std::size_t d=i+(op==71?2:3);
      if(d>=i+n)return false;
      if(w[d]==11) {
        if(d+1>=i+n)return false;
        if(w[d+1]==20 || w[d+1]==22)return false;
      }
    }
    i+=n;
  }
  return true;
}
inline uint32_t VertexSpecialization(uint32_t specialization) {return specialization&~kFlat;}
}
