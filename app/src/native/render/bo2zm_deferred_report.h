#pragma once


#include <string>

#include <rex/logging.h>

namespace bo2zm::native {
void ReportDeferred(std::string line);
}

#define BO2ZM_REPORT_DEFERRED(...) ::bo2zm::native::ReportDeferred(fmt::format(__VA_ARGS__))
