#include "generated/default/bo2cp_init.h"

#include "bo2cp_app.h"

REXCVAR_DEFINE_INT32(bo2cp_volume, 100, "BO2",
                     "PC output volume, 0-100 percent; Switch always uses normal 100 percent output")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_DEFINE_APP(bo2cp, Bo2cpApp::Create)
