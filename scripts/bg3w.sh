#!/bin/bash
# BG3SE-macOS Steam Launch Script
# Steam launch options: /path/to/bg3se-macos/scripts/bg3w.sh %command%
#
# Steam passes the .app bundle path, but we need to run the actual executable
# inside Contents/MacOS/ for DYLD_INSERT_LIBRARIES to work.

# Get script directory (works even when called via symlink or absolute path)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Debug output
echo "=== BG3W Launch Script ===" >> /tmp/bg3w_debug.log
echo "Date: $(date)" >> /tmp/bg3w_debug.log
echo "Script dir: $SCRIPT_DIR" >> /tmp/bg3w_debug.log
echo "Project root: $PROJECT_ROOT" >> /tmp/bg3w_debug.log
echo "Args: $@" >> /tmp/bg3w_debug.log

# Get the first argument (should be the .app bundle path)
APP_PATH="$1"
shift  # Remove first arg, keep any additional args

# If it's a .app bundle, extract the actual executable
if [[ "$APP_PATH" == *.app ]]; then
    EXEC_PATH="${APP_PATH}/Contents/MacOS/Baldur's Gate 3"
    echo "Detected .app bundle, using executable: $EXEC_PATH" >> /tmp/bg3w_debug.log
else
    EXEC_PATH="$APP_PATH"
    echo "Using path directly: $EXEC_PATH" >> /tmp/bg3w_debug.log
fi

# Verify executable exists
if [[ ! -f "$EXEC_PATH" ]]; then
    echo "ERROR: Executable not found: $EXEC_PATH" >> /tmp/bg3w_debug.log
    exit 1
fi

# Find dylib relative to script location
DYLIB="${PROJECT_ROOT}/build/lib/libbg3se.dylib"

if [[ ! -f "$DYLIB" ]]; then
    echo "ERROR: libbg3se.dylib not found at: $DYLIB" >> /tmp/bg3w_debug.log
    echo "ERROR: Build it first with: cd build && cmake .. && cmake --build ." >> /tmp/bg3w_debug.log
    exit 1
fi

echo "DYLIB: $DYLIB" >> /tmp/bg3w_debug.log

# Pass through BG3SE environment variables (Issue #65 diagnostics).
#
# Steam launches this wrapper, so the interactive shell's environment is NOT
# inherited: exporting BG3SE_* in a terminal has no effect on a Steam launch.
# Read them from a config file as well, which is the only way to set one when
# the game is started from the Steam library.
BG3SE_ENV_FILE="${HOME}/Library/Application Support/BG3SE/bg3se.env"
if [[ -f "$BG3SE_ENV_FILE" ]]; then
    # shellcheck disable=SC1090
    set -a; source "$BG3SE_ENV_FILE"; set +a
    echo "Sourced env file: $BG3SE_ENV_FILE" >> /tmp/bg3w_debug.log
fi

# Forward every BG3SE_* variable that is set, not a hand-maintained subset —
# the old list silently dropped new switches (BG3SE_SHADER_ALIAS among them).
BG3SE_ENVS=""
while IFS='=' read -r var _; do
    [[ -n "${!var}" ]] || continue
    BG3SE_ENVS="${BG3SE_ENVS} ${var}=${!var}"
    echo "  ${var}=${!var}" >> /tmp/bg3w_debug.log
done < <(env | grep -E '^BG3SE_[A-Z0-9_]+=' | sort)

echo "Forcing ARM64 architecture with inline DYLD_INSERT_LIBRARIES" >> /tmp/bg3w_debug.log
echo "===========================" >> /tmp/bg3w_debug.log

# Force ARM64 architecture with inline env var (export doesn't work with arch)
# shellcheck disable=SC2086
exec arch -arm64 env DYLD_INSERT_LIBRARIES="$DYLIB" $BG3SE_ENVS "$EXEC_PATH" "$@"
