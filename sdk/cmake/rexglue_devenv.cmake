add_custom_target(config-rexdevenv
    COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
        "Import-Module '${REXGLUE_ROOT}/scripts/PSReX' -Force; Invoke-ReXSetup -RepoRoot '${REXGLUE_ROOT}'"
    WORKING_DIRECTORY "${REXGLUE_ROOT}"
    COMMENT "Configuring ReXGlue developer environment..."
    VERBATIM
    USES_TERMINAL
)
