function(rex_target_pch target)
    target_precompile_headers(${target} PRIVATE
        <algorithm>
        <array>
        <cstddef>
        <cstdint>
        <cstring>
        <functional>
        <memory>
        <optional>
        <string>
        <string_view>
        <utility>
        <vector>

        <rex/platform.h>
        <rex/types.h>
        <rex/assert.h>
        <rex/cvar.h>
        <rex/logging.h>

        ${ARGN}
    )
endfunction()
