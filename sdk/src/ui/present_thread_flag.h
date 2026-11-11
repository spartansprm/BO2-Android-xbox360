// SPDX-License-Identifier: GPL-3.0-only


#pragma once

namespace rex::ui::present_thread {

// True on the dedicated "Presenter Paint" thread (present_own_thread), false on the ring/UI thread.
bool OnDedicatedPaintThread();
// Called once by Presenter::PaintThreadMain.
void MarkDedicatedPaintThread();

}  // namespace rex::ui::present_thread
