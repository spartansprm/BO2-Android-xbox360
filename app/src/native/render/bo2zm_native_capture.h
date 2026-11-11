#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace rex::ui {
class Presenter;
}

namespace bo2zm::capture {


void Boot(std::function<rex::ui::Presenter*()> get_presenter);


void RequestCapture(std::string label, std::chrono::milliseconds delay = {});


void Stop();


void CaptureNowForTests(rex::ui::Presenter* presenter, const std::string& label);

}
