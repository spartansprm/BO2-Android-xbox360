#include "user_entries.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace rex;
using namespace rex::system;
using namespace rex::kernel::xam;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "expected temporary directory");
    KernelState single(argv[1]), two(argv[1], 2), three(argv[1], 3), four(argv[1], 4);
    for (auto* kernel : {&single, &two, &three, &four}) {
      active_kernel = kernel;
      const uint32_t count = kernel == &single ? 1u : kernel == &two ? 2u : kernel == &three ? 3u : 4u;
      const bool second = count >= 2;
      be<uint64_t> xuid;
      be<uint32_t> granted;
      Check(XamUserGetXUID_entry(0, 7, &xuid) == X_E_SUCCESS && uint64_t(xuid) == 0xB13EBABEBABEBABE,
            "primary XUID API changed");
      Check(XamUserGetSigninState_entry(0) == 1, "primary local sign-in changed");
      Check(XamUserGetSigninState_entry(1) == (second ? 1u : 0u), "opt-in secondary sign-in wrong");
      Check((XamUserGetXUID_entry(1, 1, &xuid) == X_E_SUCCESS) == second, "secondary XUID lookup wrong");
      if (second) {
        Check(uint64_t(xuid) == 0xB13EBABEBABEBABF, "secondary XUID unstable");
        X_USER_SIGNIN_INFO info;
        Check(XamUserGetSigninInfo_entry(1, 0, &info) == X_E_SUCCESS && uint64_t(info.xuid) == uint64_t(xuid)
              && uint32_t(info.signin_state) == 1 && uint32_t(info.unk08) == 0, "sign-in info is not a normal local account");
        char name[16]{};
        Check(XamUserGetName_entry(1, name, sizeof(name)) == X_E_SUCCESS && std::strcmp(name, "Player 2") == 0,
              "secondary name API wrong");
        granted = 0xFFFFFFFF;
        Check(XamUserCheckPrivilege_entry(1, 0xFE, &granted) == X_ERROR_SUCCESS && uint32_t(granted) == 0,
              "secondary received a Live privilege");
        Check(XamUserGetXUID_entry(1, 2, &xuid) == X_E_NO_SUCH_USER && uint64_t(xuid) == 0,
              "secondary satisfies an online-profile-only query");
        Check(XamUserIsOnlineEnabled_entry(1) == 0, "secondary advertises online support");
        Check(XamUserGetMembershipTier_entry(1) == X_ERROR_NO_SUCH_USER, "secondary received membership");
      }
      for (uint32_t index = 0; index < count; ++index) {
        Check(XamUserGetSigninState_entry(index) == 1, "configured user is not signed in");
        Check(XamUserGetXUID_entry(index, 1, &xuid) == X_E_SUCCESS &&
              uint64_t(xuid) == 0xB13EBABEBABEBABE + index, "configured identity is not unique");
        if (index > 0) {
          char name[16]{};
          Check(XamUserGetName_entry(index, name, sizeof(name)) == X_E_SUCCESS &&
                std::string(name) == "Player " + std::to_string(index + 1), "local names overlap");
          Check(XamUserGetXUID_entry(index, 2, &xuid) == X_E_NO_SUCH_USER,
                "additional offline user satisfies online-only query");
          granted = 0xFFFFFFFF;
          Check(XamUserCheckPrivilege_entry(index, 0xFE, &granted) == X_ERROR_SUCCESS &&
                uint32_t(granted) == 0, "additional user received Live privilege");
        }
        uint64_t local_owner = 0;
        Check(ContentOwner(index, XContentType::kSavedGame, local_owner) &&
              local_owner == 0xB13EBABEBABEBABE + index, "configured save owner aliases another user");
      }
      for (uint32_t index : {2u, 3u, 4u, 255u, 0xFFFFFFFFu}) {
        if (index < count) continue;
        Check(XamUserGetSigninState_entry(index) == 0, "unconfigured user is signed in");
        Check(XamUserGetXUID_entry(index, 7, &xuid) != X_E_SUCCESS && uint64_t(xuid) == 0,
              "invalid user received another profile's XUID");
      }
      granted = 0xFFFFFFFF;
      Check(XamUserCheckPrivilege_entry(0, 0xFE, &granted) == X_ERROR_SUCCESS && uint32_t(granted) == 0,
            "primary privilege policy changed");
      Check(XamShowSigninUI_entry(4, 0x80000) == X_ERROR_SUCCESS, "offline sign-in UI failed");
      Check(kernel->notifications.size() == 2 && kernel->notifications[0] ==
            std::make_pair(0xAu, (1u << count) - 1u) && kernel->notifications[1] == std::make_pair(9u, 0u),
            "sign-in notification disagrees with query state");
      uint64_t owner = 0;
      Check(ContentOwner(0, XContentType::kSavedGame, owner) && owner == 0xB13EBABEBABEBABE,
            "primary save owner changed");
      Check(ContentOwner(1, XContentType::kSavedGame, owner) == second, "secondary save owner not indexed");
      if (second) Check(owner == 0xB13EBABEBABEBABF, "secondary save aliases primary");
      Check(ContentOwner(0xFE, XContentType::kSavedGame, owner) && owner == 0xB13EBABEBABEBABE,
            "internal content sentinel changed");
      Check(ContentOwner(1, XContentType::kMarketplaceContent, owner) && owner == 0,
            "shared DLC owner changed");
    }
    std::cout << "production XAM identities, opt-in/default behavior, denied Live privileges, notifications and save routing passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
