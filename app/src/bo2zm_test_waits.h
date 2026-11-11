#pragma once

#include <functional>
#include <string>

namespace bo2zm::test_waits {


bool Run(double scale, const std::function<void(const std::string&)>& log);


void RunIfRequested();

}
