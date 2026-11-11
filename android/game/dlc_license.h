// SPDX-License-Identifier: GPL-3.0-only


#pragma once
#include <cstdint>

namespace bo2zm::android {

inline constexpr uint32_t kDlcTrialLicense = 1;
inline constexpr uint32_t kDlcFullLicense = 0xFFFFFFFFu;

inline constexpr uint32_t DlcGameLicense(uint32_t header_license, bool mtx_package) {
  return header_license == kDlcTrialLicense && !mtx_package ? kDlcFullLicense : header_license;
}

static_assert(DlcGameLicense(1, false) == kDlcFullLicense);
static_assert(DlcGameLicense(1, true) == 1);
static_assert(DlcGameLicense(16, false) == 16 && DlcGameLicense(2, false) == 2 && DlcGameLicense(0x508, true) == 0x508);
static_assert(DlcGameLicense(0, false) == 0);

}
