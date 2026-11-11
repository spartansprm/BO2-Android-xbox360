// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_v046.h"
#include "bo2zm_native_pass_profile.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace v = bo2zm::native::v046;
static int g_failures = 0;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (!(c)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)

static void TestAspectFuse() {

  auto f = v::PlanDepthAspectFuse(0, 0, 1280, 768, v::kAspectDepth, 1280, 768, true);
  CHECK(f.fuse && f.bits == v::kLoadDepthClearOnly);
  auto loads = v::DepthSlotLoads(v::Load::kLoad, f.bits);
  CHECK(loads.depth == v::Load::kClear && loads.stencil == v::Load::kLoad);

  f = v::PlanDepthAspectFuse(0, 0, 1280, 768, v::kAspectStencil, 1280, 768, true);
  CHECK(f.fuse && f.bits == v::kLoadStencilClearOnly);
  loads = v::DepthSlotLoads(v::Load::kLoad, f.bits);
  CHECK(loads.depth == v::Load::kLoad && loads.stencil == v::Load::kClear);

  CHECK(!v::PlanDepthAspectFuse(0, 0, 1280, 768, v::kAspectDepth | v::kAspectStencil, 1280, 768, true).fuse);

  CHECK(!v::PlanDepthAspectFuse(0, 0, 1280, 720, v::kAspectDepth, 1280, 768, true).fuse);
  CHECK(!v::PlanDepthAspectFuse(0, 8, 1280, 760, v::kAspectDepth, 1280, 768, true).fuse);
  CHECK(!v::PlanDepthAspectFuse(1, 0, 1279, 768, v::kAspectDepth, 1280, 768, true).fuse);

  CHECK(!v::PlanDepthAspectFuse(0, 0, 1280, 768, v::kAspectDepth, 1280, 768, false).fuse);
  CHECK(!v::PlanDepthAspectFuse(0, 0, 1280, 768, 0, 1280, 768, true).fuse);

  loads = v::DepthSlotLoads(v::Load::kLoad, 0);
  CHECK(loads.depth == v::Load::kLoad && loads.stencil == v::Load::kLoad);
  loads = v::DepthSlotLoads(v::Load::kDontCare, 0);
  CHECK(loads.depth == v::Load::kDontCare && loads.stencil == v::Load::kDontCare);

  CHECK((v::kLoadDepthClearOnly & 0x1FF) == 0 && (v::kLoadStencilClearOnly & 0x1FF) == 0);
  CHECK(v::kLoadDepthClearOnly != v::kLoadStencilClearOnly);
}

static void TestUnreadLearner() {
  v::UnreadLearner l;

  CHECK(!l.OnCopy());
  for (uint32_t i = 1; i < v::kLazyLearnCopies; ++i) CHECK(!l.OnCopy());
  CHECK(l.OnCopy());
  CHECK(l.OnCopy());
  l.OnRead();
  CHECK(!l.OnCopy());
  for (uint32_t i = 1; i < v::kLazyLearnCopies; ++i) CHECK(!l.OnCopy());
  CHECK(l.OnCopy());

  v::UnreadLearner read;
  for (int i = 0; i < 5000; ++i) {
    CHECK(!read.OnCopy());
    read.OnRead();
  }
}

static void TestFrontLearner() {
  v::FrontLearner f;
  CHECK(!f.MayDrop());
  for (uint32_t i = 0; i < v::kLazyLearnCopies; ++i) {
    f.OnResolve();
    f.OnSwap();
  }
  CHECK(f.MayDrop());

  f.OnSwap();
  CHECK(!f.MayDrop());
  for (uint32_t i = 0; i < v::kLazyLearnCopies; ++i) {
    f.OnResolve();
    f.OnSwap();
  }
  CHECK(f.MayDrop());
  f.OnSampled();
  CHECK(!f.MayDrop());
}

static void TestSameResolve() {
  v::CopyFacts a;
  a.source_image = 0x1234;
  a.source_epoch = 7;
  a.src_x = 0;
  a.src_y = 0;
  a.width = 1303;
  a.height = 720;
  a.format = 37;
  a.dest_width = 1303;
  a.dest_height = 720;
  a.whole_dest = true;
  a.dest_address = 0x1D474000;
  v::CopyFacts b = a;
  b.dest_address = 0x1D6F8000;
  CHECK(v::SameResolve(a, b));

  v::CopyFacts c = b;
  c.source_epoch = 8;
  CHECK(!v::SameResolve(a, c));

  c = b; c.source_image = 0x9999; CHECK(!v::SameResolve(a, c));
  c = b; c.src_x = 32; CHECK(!v::SameResolve(a, c));
  c = b; c.width = 448; CHECK(!v::SameResolve(a, c));
  c = b; c.format = 26; CHECK(!v::SameResolve(a, c));
  c = b; c.dest_width = 896; CHECK(!v::SameResolve(a, c));

  c = b; c.whole_dest = false; CHECK(!v::SameResolve(a, c));
  c = b; c.dest_address = a.dest_address; CHECK(!v::SameResolve(a, c));
  CHECK(!v::SameResolve(v::CopyFacts{}, b));

  CHECK(v::ResolveDifference(a, b) == 0);
  CHECK(v::ResolveDifference(v::CopyFacts{}, b) == v::kDiffNoPrevious);
  c = b; c.source_epoch = 8; CHECK(v::ResolveDifference(a, c) == v::kDiffEpoch);
  c = b; c.source_image = 0x9999; c.width = 448; CHECK(v::ResolveDifference(a, c) == (v::kDiffSource | v::kDiffSize));
  c = b; c.dest_width = 896; CHECK(v::ResolveDifference(a, c) == v::kDiffDestSize);
  c = b; c.dest_address = a.dest_address; CHECK(v::ResolveDifference(a, c) == v::kDiffSameAddress);
  c = b; c.whole_dest = false; CHECK(v::ResolveDifference(a, c) == v::kDiffNotWhole);
}

static void TestFitAreaToClear() {
  v::DrawnExtent e;
  e.right = 1280; e.bottom = 720; e.draws = v::kFitLearnDraws;

  CHECK(v::FitAreaToClear(e, 0, 0, 1280, 720, 1280, 768));

  v::DrawnExtent s; s.right = 960; s.bottom = 1024; s.draws = 5000;
  CHECK(v::FitAreaToClear(s, 0, 0, 960, 1024, 1040, 1024));

  v::DrawnExtent t = e; t.bottom = 721; CHECK(!v::FitAreaToClear(t, 0, 0, 1280, 720, 1280, 768));
  t = s; t.right = 961; CHECK(!v::FitAreaToClear(t, 0, 0, 960, 1024, 1040, 1024));

  t = e; t.draws = v::kFitLearnDraws - 1; CHECK(!v::FitAreaToClear(t, 0, 0, 1280, 720, 1280, 768));
  t = e; t.banned = true; CHECK(!v::FitAreaToClear(t, 0, 0, 1280, 720, 1280, 768));
  CHECK(!v::FitAreaToClear(e, 0, 16, 1280, 704, 1280, 768));
  CHECK(!v::FitAreaToClear(e, 0, 0, 1280, 768, 1280, 768));
  CHECK(!v::FitAreaToClear(e, 0, 0, 1280, 800, 1280, 768));
  CHECK(!v::FitAreaToClear(e, 0, 0, 0, 720, 1280, 768));
}

static void TestPassProfileLoadMode() {
  namespace pp = bo2zm::native::pass_profile;
  pp::enabled.store(true);
  pp::Job job;
  job.sampled = true;
  pp::Meta meta{3, 1280, 768, 0, 0xABCD};
  meta.formats = {0, 0, 0, 0, 129};
  meta.load = v::kLoadDepthClearOnly;
  CHECK(job.Begin(meta, 0));
  CHECK(job.passes[0].meta.load == v::kLoadDepthClearOnly);

  pp::Meta other = meta;
  other.load = 0;
  CHECK(!(other == job.passes[0].meta));
}

int main() {
  TestAspectFuse();
  TestUnreadLearner();
  TestFrontLearner();
  TestSameResolve();
  TestFitAreaToClear();
  TestPassProfileLoadMode();
  if (g_failures) {
    std::fprintf(stderr, "test_android_v046: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v046: all checks passed\n");
  return EXIT_SUCCESS;
}
