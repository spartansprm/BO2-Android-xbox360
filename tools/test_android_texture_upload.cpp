// SPDX-License-Identifier: GPL-3.0-only
#include "texture_tests/bc_fixtures.h"
#include "bo2zm_native_texture_upload.h"
#include "bo2zm_native_shader_profile.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

void Require(bool value) { if (!value) throw std::runtime_error("Texture upload check failed"); }
int main(int argc, char** argv) {
  using namespace bo2zm::texture;
  unsigned chains = 0, images = 0;
  for (auto format : {BcFormat::BC1,BcFormat::BC2,BcFormat::BC3,BcFormat::BC4,BcFormat::BC5}) {
    for (auto shape : {std::array<uint32_t,4>{16,16,6,5}, {7,5,6,3}, {32,8,1,6}, {1,1,1,1}}) {
      auto [w,h,layers,levels] = shape;
      const auto fixture = bo2test::MakeBcMipFixture(format,w,h,layers,levels);
      BcMipUpload result;
      Require(ConvertBcMips(format,w,h,layers,levels,fixture.blocks,result));
      Require(result.pixels==fixture.rgba);
      Require(result.layout.block_bytes==fixture.blocks.size());
      Require(result.layout.pixel_bytes==fixture.rgba.size());
      if (w==16) {
        constexpr size_t rgba_offsets[] = {0,6144,7680,8064,8160};
        constexpr size_t bc8_offsets[] = {0,768,960,1008,1056};
        for (unsigned n=0;n<5;++n) {
          Require(result.layout.pixel_offsets[n]==rgba_offsets[n]);
          Require(result.layout.block_offsets[n]==bc8_offsets[n]*(BcBlockBytes(format)/8));
        }
        Require(result.layout.pixel_bytes==8184);
      }
      std::array<uint32_t,16> offsets{};
      for (unsigned n=0;n<levels;++n) offsets[n]=uint32_t(result.layout.pixel_offsets[n]);
      std::array<VkBufferImageCopy,16> copies{};
      Require(TextureCopyRegions(w,h,layers,1,levels,offsets,128,copies,4));
      for (unsigned n=0;n<levels;++n) {
        Require(copies[n].bufferOffset==128+offsets[n]);
        Require(copies[n].imageSubresource.mipLevel==n && copies[n].imageSubresource.layerCount==layers);
        Require(copies[n].imageExtent.width==std::max(w>>n,1u));
        Require(copies[n].imageExtent.height==std::max(h>>n,1u));
      }
      auto prior=result.pixels;
      Require(!ConvertBcMips(format,w,h,layers,levels,std::span(fixture.blocks).first(fixture.blocks.size()-1),result));
      Require(result.pixels==prior);
      Require(!ConvertBcMips(format,w,h,layers,levels,fixture.blocks,result,fixture.rgba.size()-1));
      Require(result.pixels==prior);
      Require(!TextureCopyRegions(w,h,layers,1,levels,offsets,3,copies,4));
      Require(!TextureCopyRegions(w,h,layers,1,levels,offsets,UINT64_MAX-3,copies) || levels==1);
      ++chains; images+=layers*levels;
    }
  }
  BcMipLayout invalid;
  Require(!PlanBcMips(BcFormat::BC1,UINT32_MAX,UINT32_MAX,6,16,invalid));
  Require(!PlanBcMips(BcFormat::BC1,1,1,1,2,invalid));
  Require(!PlanBcMips(BcFormat::BC1,0,1,1,1,invalid));
  Require(!PlanBcMips(BcFormat::BC1,1,1,0,1,invalid));

  std::array<uint32_t,3> r8_offsets{0,35,41}; std::array<VkBufferImageCopy,16> r8_regions{};
  Require(TextureCopyRegions(7,5,1,1,3,r8_offsets,16,r8_regions));
  Require(!TextureCopyRegions(7,5,1,1,3,r8_offsets,16,r8_regions,4));


  for (int i=1;i<argc;++i) {
    std::ifstream f(argv[i],std::ios::binary|std::ios::ate);
    Require(bool(f)); const auto size=f.tellg(); Require(size>0 && size%4==0);
    std::vector<uint32_t> words(size_t(size)/4); f.seekg(0); f.read(reinterpret_cast<char*>(words.data()),size);
    Require(bo2zm::native::UboOnlySpirv(words)==(i==1));
    if (i==1) { words.back()=0; words.resize(6); Require(!bo2zm::native::UboOnlySpirv(words)); }
  }
  std::cout << chains << " mip chains, " << images << " subresources: golden bytes, offsets, regions and bounds passed; shader profile gates passed\n";
}
