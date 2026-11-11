#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
static std::string read(const char* p) { std::ifstream f(p); if(!f) throw std::runtime_error(p); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()}; }
static void contains(const std::string& s, std::string_view x) { if(s.find(x)==std::string::npos) throw std::runtime_error(std::string(x)); }
int main() { try {
 const auto game=read("android/app/src/main/java/org/bo2port/diagnostic/GameActivity.java");
 const auto entry=read("android/game/entry.cpp");
 const auto draw=read("app/src/native/render/bo2zm_native_draws.cpp");
 const auto target=read("app/src/native/render/bo2zm_native_targets.cpp");
 for(auto key: {"disable_scene_pass_dependencies","disable_scene_transfer_barriers","disable_scene_output_read_barrier"}){contains(game,key);}
 for(auto key: {"scene_image_dependencies_enabled","scene_transfer_barriers_enabled","resolved_output_barrier_enabled"})contains(entry,key);
 for(auto key: {"bo2zm_native_scene_pass_dependencies","bo2zm_native_scene_transfer_barriers","bo2zm_native_scene_output_read_barrier"})contains(target,key);
 contains(draw,"REXCVAR_GET(bo2zm_native_scene_pass_dependencies)");
 std::puts("v0.32 independent Vulkan scene sync A/B source contract: PASS");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"ERROR: %s\\n",e.what());return 1;}}
