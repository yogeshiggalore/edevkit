# =============================================================================
# edevkit — shell helpers (bash / zsh)
# =============================================================================
# This file is the source-of-truth for the edev_* helpers. It is loaded
# automatically whenever you activate the project venv:
#
#   source "$EDEV_VENV/bin/activate"     # appends ↓ run env_helpers.sh
#                                        # (Pattern C — see installation.html)
#
# The append-on-activate wiring is set up once by tools/setup_venv.sh.
# This file stays in the repo, so `git pull` updates the helpers without
# touching the venv.
#
# Pre-req: the four layout env vars from installation.html → "Pick a layout"
# must already be exported in your shell rc:
#
#   EDEVKIT_REPO            — clone of this git repo
#   EDEV_VENV               — Python virtual environment
#   ZEPHYR_WORKSPACE        — west workspace (zephyr/, modules/, mcuboot/, ...)
#   ZEPHYR_SDK_INSTALL_DIR  — Zephyr toolchains
#
# Cross-references the original edevtoolkit/documents/env_script.txt pattern
# this is based on. Differences from edevtoolkit:
#   - Default board:    rpi_pico2/rp2350a/m33 (RP2350B-class M33 cores),
#                       not rpi_pico (RP2040)
#   - UF2 volume:       RP2350,    not RPI-RP2
#   - Variable names:   EDEVKIT_REPO / EDEV_VENV / ZEPHYR_WORKSPACE
#                       (not ZEPHYR_WORKSPACE_EDEV)
#   - Adds:             edev_flash (probe-rs), edev_flash_uf2 (BOOTSEL),
#                       edev_ota, edev_console
#   - Idempotent:       safe to source multiple times.
# =============================================================================

# ─── exported defaults ────────────────────────────────────────────────────────
# Zephyr 3.7+ requires fully-qualified board targets when a board has multiple
# cores / variants. RP2350 ships with both Cortex-M33 and Hazard3 cores;
# edevkit1 uses the M33 cores, so the default is the M33 variant.
export EDEV_BOARD_DEFAULT="${EDEV_BOARD_DEFAULT:-rpi_pico2/rp2350a/m33}"
export EDEV_UF2_VOLUME="${EDEV_UF2_VOLUME:-RP2350}"

# ─── _edev_board_dir — path-safe form of a board target ──────────────────────
# Zephyr uses '/' in fully-qualified board targets (e.g. rpi_pico2/rp2350a/m33).
# Build directories can't contain those slashes, so we substitute '_'.
_edev_board_dir () { printf '%s' "${1//\//_}"; }

# Add pioasm to PATH if it exists (built per installation.html step 6)
if [ -x "${EDEVKIT_REPO}/tools/pico-sdk-tools/pioasm/build/pioasm" ]; then
    case ":$PATH:" in
        *":${EDEVKIT_REPO}/tools/pico-sdk-tools/pioasm/build:"*) ;;
        *) export PATH="${EDEVKIT_REPO}/tools/pico-sdk-tools/pioasm/build:$PATH" ;;
    esac
fi

# ─── _edev_check — sanity-check the four layout vars before any helper runs ──
_edev_check () {
    local missing=""
    [ -z "${EDEVKIT_REPO:-}" ]            && missing="$missing EDEVKIT_REPO"
    [ -z "${EDEV_VENV:-}" ]               && missing="$missing EDEV_VENV"
    [ -z "${ZEPHYR_WORKSPACE:-}" ]        && missing="$missing ZEPHYR_WORKSPACE"
    [ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ]  && missing="$missing ZEPHYR_SDK_INSTALL_DIR"
    if [ -n "$missing" ]; then
        echo "❌ edevkit env not set up. Missing:$missing" >&2
        echo "   See tools/web/pages/installation.html step 'Pick a layout'." >&2
        return 1
    fi
}

# ─── edev_activate — enter the Python venv ───────────────────────────────────
edev_activate () {
    _edev_check || return 1
    if [ ! -f "$EDEV_VENV/bin/activate" ]; then
        echo "❌ venv not found at $EDEV_VENV — run 'python3 -m venv \"\$EDEV_VENV\"' first." >&2
        return 1
    fi
    # shellcheck disable=SC1090
    source "$EDEV_VENV/bin/activate"
}

# ─── westz — run west from inside the Zephyr workspace ───────────────────────
# West needs to resolve modules relative to the workspace; this wrapper means
# you can invoke `westz build …` from any directory without cd-ing first.
westz () {
    _edev_check || return 1
    ( cd "$ZEPHYR_WORKSPACE" && west "$@" )
}

# ─── edev_create_app — scaffold a new Zephyr app in the current directory ───
# Usage: edev_create_app <name> [parent_dir]
#   defaults parent to $PWD (the current working directory)
# Mirrors edevtoolkit's edev_create_app, with sysbuild-friendly layout.
edev_create_app () {
    _edev_check || return 1
    local name="$1"
    local parent="${2:-$PWD}"
    if [ -z "$name" ]; then
        echo "Usage: edev_create_app <name> [parent_dir]" >&2
        return 1
    fi
    local app_dir="$parent/$name"
    if [ -e "$app_dir" ]; then
        echo "❌ $app_dir already exists" >&2
        return 1
    fi

    echo "📦 Creating Zephyr app: $app_dir"
    mkdir -p "$app_dir/src" "$app_dir/boards"
    # Zephyr looks for board overlays under boards/ matching the FQBN with
    # '/' replaced by '_' (e.g. rpi_pico2_rp2350a_m33.overlay).
    : > "$app_dir/boards/$(_edev_board_dir "$EDEV_BOARD_DEFAULT").overlay"

    cat > "$app_dir/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS \$ENV{ZEPHYR_BASE})
project(${name})

target_sources(app PRIVATE src/main.c)
EOF

    cat > "$app_dir/prj.conf" <<'EOF'
CONFIG_LOG=y
CONFIG_LOG_MODE_DEFERRED=y
CONFIG_SHELL=y
EOF

    cat > "$app_dir/src/main.c" <<'EOF'
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("hello from edevkit");
    return 0;
}
EOF

    cat > "$app_dir/.gitignore" <<'EOF'
build/
build-*/
EOF

    echo "✅ Created $app_dir"
    echo "   Next:  cd \"$app_dir\" && edev_build && edev_flash_uf2"
}

# ─── edev_build — pristine build using sysbuild (MCUboot + app) ───────────────
# Usage: edev_build [src] [board]
#   src   defaults to $PWD
#   board defaults to $EDEV_BOARD_DEFAULT  (rpi_pico2/rp2350a/m33)
edev_build () {
    _edev_check || return 1
    local src="${1:-$PWD}"
    local board="${2:-$EDEV_BOARD_DEFAULT}"
    local builddir="${src}/build-$(_edev_board_dir "$board")"

    echo "🧱 Building (board: $board)"
    echo "   Source: $src"
    echo "   Build:  $builddir"

    westz build -p always -b "$board" --sysbuild -s "$src" -d "$builddir"
}

# ─── edev_build_simple — build without sysbuild (single image, no MCUboot) ───
# For early bring-up / simple samples that don't need OTA.
edev_build_simple () {
    _edev_check || return 1
    local src="${1:-$PWD}"
    local board="${2:-$EDEV_BOARD_DEFAULT}"
    local builddir="${src}/build-$(_edev_board_dir "$board")"

    echo "🧱 Building (simple, board: $board)"
    westz build -p always -b "$board" -s "$src" -d "$builddir"
}

# ─── edev_flash — flash via probe-rs (SWD probe required) ────────────────────
# Used during normal dev when a probe is connected. For BOOTSEL/UF2,
# use edev_flash_uf2 instead.
edev_flash () {
    _edev_check || return 1
    local src="${1:-$PWD}"
    local board="${2:-$EDEV_BOARD_DEFAULT}"
    local builddir="${src}/build-$(_edev_board_dir "$board")"
    local image_name="$(basename "$src")"     # sysbuild names the per-image dir after the CMake project()
    local mcuboot_bin="${builddir}/mcuboot/zephyr/zephyr.bin"
    local app_bin="${builddir}/${image_name}/zephyr/zephyr.signed.bin"
    # Fallback for non-sysbuild builds:
    [ -f "$app_bin" ] || app_bin="${builddir}/zephyr/zephyr.bin"

    if ! command -v probe-rs >/dev/null 2>&1; then
        echo "❌ probe-rs not found on PATH. Install via 'cargo install --locked probe-rs-tools'." >&2
        return 1
    fi

    echo "🚀 Flashing via probe-rs (chip: RP2350)"
    if [ -f "$mcuboot_bin" ]; then
        probe-rs download --chip RP2350 --binary-format bin --base-address 0x10000000 "$mcuboot_bin" || return $?
        probe-rs download --chip RP2350 --binary-format bin --base-address 0x10040000 "$app_bin"     || return $?
    else
        probe-rs download --chip RP2350 --binary-format bin --base-address 0x10000000 "$app_bin"     || return $?
    fi
    echo "✅ flashed via probe-rs"
}

# ─── edev_flash_uf2 — UF2 drag-and-drop flash for BOOTSEL mode ───────────────
# Hold BOOTSEL while plugging USB → kit appears as /Volumes/RP2350 (macOS) or
# the equivalent mount on Linux. This helper copies the signed UF2 across.
edev_flash_uf2 () {
    _edev_check || return 1
    local src="${1:-$PWD}"
    local board="${2:-$EDEV_BOARD_DEFAULT}"
    local builddir="${src}/build-$(_edev_board_dir "$board")"
    local image_name="$(basename "$src")"
    local uf2="${builddir}/${image_name}/zephyr/zephyr.signed.uf2"
    [ -f "$uf2" ] || uf2="${builddir}/zephyr/zephyr.uf2"

    [ -f "$uf2" ] || { echo "❌ UF2 not found: $uf2 (run edev_build first)" >&2; return 1; }

    local mountpoint="/Volumes/${EDEV_UF2_VOLUME}"
    if [ -d "/media/$USER/${EDEV_UF2_VOLUME}" ]; then
        mountpoint="/media/$USER/${EDEV_UF2_VOLUME}"   # Linux default
    fi

    echo "🚀 UF2 drag-and-drop flash"
    echo "   File:  $uf2"
    echo "   Mount: $mountpoint"

    local waited=0
    while [ ! -d "$mountpoint" ] && [ "$waited" -lt 30 ]; do
        sleep 1; waited=$((waited + 1))
    done
    [ -d "$mountpoint" ] || {
        echo "❌ $mountpoint did not appear within 30s — hold BOOTSEL and replug." >&2
        return 1
    }

    cp "$uf2" "$mountpoint/" || return $?
    echo "✅ Copied $(basename "$uf2") → $mountpoint"
    echo "   Kit will reboot into the new firmware automatically."
}

# ─── edev_ota — push a signed image to a running edevkit via mcumgr ──────────
# Requires: mcumgr CLI (Go-based) configured with a connection profile named
# 'edevkit'. See installation.html step 11 for setup.
edev_ota () {
    _edev_check || return 1
    local src="${1:-$PWD}"
    local board="${2:-$EDEV_BOARD_DEFAULT}"
    local builddir="${src}/build-$(_edev_board_dir "$board")"
    local image_name="$(basename "$src")"
    local app_bin="${builddir}/${image_name}/zephyr/zephyr.signed.bin"
    [ -f "$app_bin" ] || app_bin="${builddir}/zephyr/zephyr.signed.bin"

    [ -f "$app_bin" ] || { echo "❌ signed image not found: $app_bin" >&2; return 1; }
    if ! command -v mcumgr >/dev/null 2>&1; then
        echo "❌ mcumgr not found. See installation.html step 11b." >&2
        return 1
    fi

    echo "📡 Uploading signed image to running kit (profile: edevkit)…"
    mcumgr -c edevkit image upload "$app_bin" || return $?
    echo "✅ upload done. Run 'mcumgr -c edevkit image test <hash>' then 'mcumgr -c edevkit reset' to swap."
}

# ─── edev_console — open a serial console to the kit ─────────────────────────
# Picks the first /dev/cu.usbmodem* (macOS) or /dev/ttyACM* (Linux) by default;
# pass an explicit path to override.
edev_console () {
    local port="${1:-}"
    if [ -z "$port" ]; then
        for cand in /dev/cu.usbmodem* /dev/ttyACM*; do
            [ -e "$cand" ] && { port="$cand"; break; }
        done
    fi
    [ -n "$port" ] || { echo "❌ no serial device found. Plug in the kit." >&2; return 1; }

    if command -v tio >/dev/null 2>&1; then
        tio "$port"
    elif command -v screen >/dev/null 2>&1; then
        screen "$port" 115200
    else
        echo "❌ neither tio nor screen found. Install one (brew install tio / apt install tio)." >&2
        return 1
    fi
}

# ─── edev_doc — open the docs hub in the default browser ─────────────────────
# Convenience for "show me the docs". Starts the local server in the background
# if it isn't already running, then opens the hub URL.
edev_doc () {
    _edev_check || return 1
    local port="${1:-8765}"
    if ! curl -fs "http://127.0.0.1:${port}/api/health" >/dev/null 2>&1; then
        echo "🚀 Starting docs hub on port $port (background)…"
        ( cd "$EDEVKIT_REPO/tools/web" && \
          nohup "$EDEV_VENV/bin/uvicorn" server:app --host 127.0.0.1 --port "$port" \
            >/tmp/edev_doc.log 2>&1 & disown ) >/dev/null 2>&1
        sleep 2
    fi
    case "$(uname -s)" in
        Darwin) open "http://127.0.0.1:${port}/" ;;
        Linux)  xdg-open "http://127.0.0.1:${port}/" >/dev/null 2>&1 ;;
        *)      echo "Open http://127.0.0.1:${port}/ in your browser." ;;
    esac
}

# ─── edev_help — list all helpers in this script ─────────────────────────────
edev_help () {
    cat <<'EOF'
edevkit shell helpers
─────────────────────
  edev_activate         enter the Python venv ($EDEV_VENV)
  westz <args…>         run `west` from inside the Zephyr workspace
  edev_create_app <n>   scaffold a new Zephyr app at $PWD/<n>/ (override: arg 2)
  edev_build [src] [b]  pristine sysbuild build (MCUboot + app)
  edev_build_simple     pristine non-sysbuild build (single image)
  edev_flash [src] [b]  flash via probe-rs (SWD probe attached)
  edev_flash_uf2        UF2 drag-and-drop flash (kit in BOOTSEL mode)
  edev_ota              push signed image via mcumgr (running kit)
  edev_console [port]   open serial console (tio if available, else screen)
  edev_doc              open the local docs hub in a browser
  edev_help             this message

Defaults:
  EDEV_BOARD_DEFAULT = $EDEV_BOARD_DEFAULT
  EDEV_UF2_VOLUME    = $EDEV_UF2_VOLUME
  EDEVKIT_REPO       = $EDEVKIT_REPO
  EDEV_VENV          = $EDEV_VENV
  ZEPHYR_WORKSPACE   = $ZEPHYR_WORKSPACE
  ZEPHYR_SDK_INSTALL_DIR = $ZEPHYR_SDK_INSTALL_DIR
EOF
}
