// SPDX-License-Identifier: GPL-3.0-only


#include "texture_tests/bc_fixtures.h"
#include "bo2zm_native_texture_upload.h"
#include <iostream>
#include <stdexcept>

using namespace bo2zm::texture;
void Require(bool value) { if (!value) throw std::runtime_error("Compact BC check failed"); }

int main() {
  unsigned chains = 0, images = 0;
  uint64_t compact_bytes = 0, rgba_bytes = 0;
  for (auto format : {BcFormat::BC1, BcFormat::BC2, BcFormat::BC3, BcFormat::BC4, BcFormat::BC5}) {
    const size_t channels = format == BcFormat::BC4 ? 1 : format == BcFormat::BC5 ? 2 : 4;
    Require(BcPixelBytes(format, BcStorage::Compact) == channels);
    const VkFormat expected_format = channels == 1 ? VK_FORMAT_R8_UNORM :
        channels == 2 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    Require(BcHostFormat(format, BcStorage::Compact) == expected_format);
    for (auto shape : {std::array<uint32_t,4>{16,16,6,5}, {7,5,6,3}, {32,8,1,6},
                       {1,1,1,1}, {9,3,6,4}, {3,3,1,2}, {1,32,6,6}}) {
      const auto [w,h,layers,levels] = shape;
      const auto fixture = bo2test::MakeBcMipFixture(format,w,h,layers,levels);
      BcMipUpload output;
      Require(ConvertBcMips(format,w,h,layers,levels,fixture.blocks,output,kBcUploadLimit,BcStorage::Compact));
      Require(output.layout.block_bytes == fixture.blocks.size());
      Require(output.pixels.size() == output.layout.pixel_bytes);
      std::array<uint32_t,16> offsets{};
      for (uint32_t n=0;n<levels;++n) {
        Require(output.layout.pixel_offsets[n] % 4 == 0);
        offsets[n] = uint32_t(output.layout.pixel_offsets[n]);
        for (uint32_t layer=0;layer<layers;++layer) {
          const auto& golden=fixture.images[n*layers+layer];
          const size_t bytes=golden.rgba.size()/4*channels;
          const size_t offset=offsets[n]+layer*output.layout.pixel_layer_bytes[n];
          Require(output.layout.pixel_layer_bytes[n] == bytes);
          for (size_t p=0;p<golden.rgba.size()/4;++p) for (size_t c=0;c<channels;++c)
            Require(output.pixels[offset+p*channels+c] == golden.rgba[p*4+c]);
          std::vector<uint8_t> guarded(bytes+32,0xcd);
          auto destination=std::span(guarded).subspan(16,bytes);
          Require(DecodeBc(format,std::max(w>>n,1u),std::max(h>>n,1u),golden.blocks,destination,BcStorage::Compact));
          Require(std::equal(destination.begin(),destination.end(),output.pixels.begin()+offset));
          Require(std::all_of(guarded.begin(),guarded.begin()+16,[](auto b){return b==0xcd;}));
          Require(std::all_of(guarded.end()-16,guarded.end(),[](auto b){return b==0xcd;}));
          std::fill(guarded.begin(),guarded.end(),0xcd);
          Require(!DecodeBc(format,std::max(w>>n,1u),std::max(h>>n,1u),
                            std::span(golden.blocks).first(golden.blocks.size()-1),destination,BcStorage::Compact));
          Require(std::all_of(guarded.begin(),guarded.end(),[](auto b){return b==0xcd;}));
          ++images;
        }
      }
      std::array<VkBufferImageCopy,16> copies{};
      Require(TextureCopyRegions(w,h,layers,1,levels,offsets,128,copies,4));
      for (uint32_t n=0;n<levels;++n) {
        Require(copies[n].bufferOffset==128+offsets[n]);
        Require(copies[n].imageSubresource.layerCount==layers && copies[n].imageSubresource.mipLevel==n);
      }
      const auto prior=output.pixels;
      Require(!ConvertBcMips(format,w,h,layers,levels,fixture.blocks,output,prior.size()-1,BcStorage::Compact));
      Require(output.pixels==prior);
      Require(!ConvertBcMips(format,w,h,layers,levels,std::span(fixture.blocks).first(fixture.blocks.size()-1),
                            output,kBcUploadLimit,BcStorage::Compact));
      Require(output.pixels==prior);
      compact_bytes+=prior.size(); rgba_bytes+=fixture.rgba.size(); ++chains;
    }
    BcMipLayout invalid;
    Require(!PlanBcMips(format,UINT32_MAX,UINT32_MAX,6,16,invalid,kBcUploadLimit,BcStorage::Compact));
    Require(!PlanBcMips(format,1,1,0,1,invalid,kBcUploadLimit,BcStorage::Compact));
    Require(!PlanBcMips(format,1,1,1,2,invalid,kBcUploadLimit,BcStorage::Compact));
  }
  Require(BcPixelBytes(BcFormat::BC4,static_cast<BcStorage>(99))==0);
  Require(BcHostFormat(static_cast<BcFormat>(99),BcStorage::Compact)==VK_FORMAT_UNDEFINED);

  BcMipLayout tail;
  Require(PlanBcMips(BcFormat::BC4,3,3,6,2,tail,kBcUploadLimit,BcStorage::Compact));
  Require(tail.pixel_offsets[1]==56 && tail.pixel_bytes==62);

  for (auto f : {BcFormat::BC4,BcFormat::BC5}) {
    BcMipLayout a,b;
    Require(PlanBcMips(f,1024,1024,1,11,a));
    Require(PlanBcMips(f,1024,1024,1,11,b,kBcUploadLimit,BcStorage::Compact));
    std::cout << (f==BcFormat::BC4?"BC4":"BC5") << " 1024 mip chain: " << a.pixel_bytes << " -> " << b.pixel_bytes << " bytes\n";
  }
  std::cout << chains << " compact mip chains, " << images << " golden subresources passed; "
            << compact_bytes << " vs " << rgba_bytes << " fixture bytes; guards, alignment and limits passed\n";
}
