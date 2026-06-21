# =============================================================================
# edevkit — shell helpers (PowerShell, Windows native)
# =============================================================================
# Mirrors tools/env_helpers.sh for Windows users.
# Dot-source from your $PROFILE AFTER setting the four layout env vars (see
# tools/web/pages/installation.html step "Pick a layout").
#
#   $env:EDEV_HOME              = "$HOME\projects\temp\edevkit"
#   $env:EDEVKIT_REPO           = "$env:EDEV_HOME\edevkit"
#   $env:EDEV_VENV              = "$env:EDEV_HOME\edev"
#   $env:ZEPHYR_WORKSPACE       = "$HOME\projects\temp\zephyrproject"
#   $env:ZEPHYR_SDK_INSTALL_DIR = "$HOME\zephyr-sdk-1.0.1"
#   . "$env:EDEVKIT_REPO\tools\env_helpers.ps1"
# =============================================================================

# Zephyr 3.7+ requires fully-qualified board targets when a board has multiple
# cores / variants. RP2350 ships with both Cortex-M33 and Hazard3 cores;
# edevkit1 uses the M33 cores, so the default is the M33 variant.
if (-not $env:EDEV_BOARD_DEFAULT) { $env:EDEV_BOARD_DEFAULT = "rpi_pico2/rp2350a/m33" }
if (-not $env:EDEV_UF2_VOLUME)    { $env:EDEV_UF2_VOLUME    = "RP2350" }

# Build directories can't contain '/'; substitute '_' for path-safe form.
function _edev_board_dir { param([string]$Board) return $Board.Replace('/', '_') }

# add pioasm to PATH if present
$pioasmDir = "$env:EDEVKIT_REPO\tools\pico-sdk-tools\pioasm\build"
if ((Test-Path $pioasmDir) -and ($env:Path -notlike "*$pioasmDir*")) {
    $env:Path = "$pioasmDir;$env:Path"
}

function _edev_check {
    $missing = @()
    if (-not $env:EDEVKIT_REPO)            { $missing += "EDEVKIT_REPO" }
    if (-not $env:EDEV_VENV)               { $missing += "EDEV_VENV" }
    if (-not $env:ZEPHYR_WORKSPACE)        { $missing += "ZEPHYR_WORKSPACE" }
    if (-not $env:ZEPHYR_SDK_INSTALL_DIR)  { $missing += "ZEPHYR_SDK_INSTALL_DIR" }
    if ($missing.Count -gt 0) {
        Write-Host "❌ edevkit env not set up. Missing: $($missing -join ', ')" -ForegroundColor Red
        Write-Host "   See tools/web/pages/installation.html step 'Pick a layout'."
        return $false
    }
    return $true
}

function edev_activate {
    if (-not (_edev_check)) { return }
    $activate = "$env:EDEV_VENV\Scripts\Activate.ps1"
    if (-not (Test-Path $activate)) {
        Write-Host "❌ venv not found at $env:EDEV_VENV — run 'python -m venv `$env:EDEV_VENV' first." -ForegroundColor Red
        return
    }
    . $activate
}

function westz {
    if (-not (_edev_check)) { return }
    Push-Location $env:ZEPHYR_WORKSPACE
    try   { west @args }
    finally { Pop-Location }
}

function edev_create_app {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [string]$Parent = "$PWD"
    )
    if (-not (_edev_check)) { return }
    $appDir = Join-Path $Parent $Name
    if (Test-Path $appDir) { Write-Host "❌ $appDir already exists" -ForegroundColor Red; return }

    Write-Host "📦 Creating Zephyr app: $appDir"
    New-Item -ItemType Directory -Force -Path "$appDir\src" | Out-Null
    New-Item -ItemType Directory -Force -Path "$appDir\boards" | Out-Null
    # Zephyr looks for board overlays under boards/ matching the FQBN with
    # '/' replaced by '_' (e.g. rpi_pico2_rp2350a_m33.overlay).
    $overlayName = (_edev_board_dir $env:EDEV_BOARD_DEFAULT) + ".overlay"
    New-Item -ItemType File -Force -Path "$appDir\boards\$overlayName" | Out-Null

    @"
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS `$ENV{ZEPHYR_BASE})
project($Name)

target_sources(app PRIVATE src/main.c)
"@ | Set-Content -Path "$appDir\CMakeLists.txt"

    @"
CONFIG_LOG=y
CONFIG_LOG_MODE_DEFERRED=y
CONFIG_SHELL=y
"@ | Set-Content -Path "$appDir\prj.conf"

    @"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF(`"hello from edevkit`");
    return 0;
}
"@ | Set-Content -Path "$appDir\src\main.c"

    "build/`nbuild-*/" | Set-Content -Path "$appDir\.gitignore"

    Write-Host "✅ Created $appDir" -ForegroundColor Green
    Write-Host "   Next:  cd $appDir; edev_build; edev_flash"
}

# Convert sysbuild's merged.hex → merged.uf2 (single drag-and-drop for BOOTSEL).
# Family ID RP2XXX_ABSOLUTE (0xe48bff57) — matches Zephyr bin2uf2 and is the
# right family for a multi-region image (bootloader + app slot). No-op when
# merged.hex isn't present (single-image build).
function _edev_emit_merged_uf2 {
    param([string]$BuildDir)
    foreach ($hex in @("$BuildDir\merged.hex", "$BuildDir\zephyr\merged.hex")) {
        if (-not (Test-Path $hex)) { continue }
        $uf2  = "$BuildDir\merged.uf2"
        $conv = "$env:EDEVKIT_REPO\tools\uf2\uf2conv.py"
        if (-not (Test-Path $conv)) {
            Write-Host "⚠️  uf2conv.py missing at $conv — skipping merged.uf2 emit." -ForegroundColor Yellow
            return
        }
        Write-Host "🧬 Emitting merged.uf2 from $(Split-Path -Leaf $hex)"
        & python $conv -c -f RP2XXX_ABSOLUTE -o $uf2 $hex | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "⚠️  merged.uf2 conversion failed; SWD flash still works." -ForegroundColor Yellow
            return
        }
        Write-Host "   → $uf2"
        return
    }
}

function edev_build {
    param([string]$Src = $PWD, [string]$Board = $env:EDEV_BOARD_DEFAULT)
    if (-not (_edev_check)) { return }
    $buildDir = "$Src\build-$(_edev_board_dir $Board)"
    Write-Host "🧱 Building (board: $Board)"
    westz build -p always -b $Board --sysbuild -s $Src -d $buildDir @args
    if ($LASTEXITCODE -ne 0) { return }
    _edev_emit_merged_uf2 $buildDir
}

function edev_build_simple {
    param([string]$Src = $PWD, [string]$Board = $env:EDEV_BOARD_DEFAULT)
    if (-not (_edev_check)) { return }
    $buildDir = "$Src\build-$(_edev_board_dir $Board)"
    westz build -p always -b $Board -s $Src -d $buildDir @args
}

function edev_flash {
    param([string]$Src = $PWD, [string]$Board = $env:EDEV_BOARD_DEFAULT)
    if (-not (_edev_check)) { return }
    $buildDir  = "$Src\build-$(_edev_board_dir $Board)"
    $imageName = (Split-Path -Leaf $Src)   # sysbuild names the per-image dir after the CMake project()
    $mcuboot   = "$buildDir\mcuboot\zephyr\zephyr.bin"
    $appBin    = "$buildDir\$imageName\zephyr\zephyr.signed.bin"
    # Fallbacks: sysbuild without MCUboot signing, then plain (non-sysbuild) build.
    if (-not (Test-Path $appBin)) { $appBin = "$buildDir\$imageName\zephyr\zephyr.bin" }
    if (-not (Test-Path $appBin)) { $appBin = "$buildDir\zephyr\zephyr.bin" }

    if (-not (Get-Command probe-rs -ErrorAction SilentlyContinue)) {
        Write-Host "❌ probe-rs not on PATH. Install via 'cargo install --locked probe-rs-tools'." -ForegroundColor Red
        return
    }
    Write-Host "🚀 Flashing via probe-rs (chip: RP2350)"
    if (Test-Path $mcuboot) {
        probe-rs download --chip RP2350 --binary-format bin --base-address 0x10000000 $mcuboot
        probe-rs download --chip RP2350 --binary-format bin --base-address 0x10040000 $appBin
    } else {
        probe-rs download --chip RP2350 --binary-format bin --base-address 0x10000000 $appBin
    }
}

function edev_help {
    Write-Host "edevkit shell helpers (PowerShell)"
    Write-Host "─────────────────────────────────"
    Write-Host "  edev_activate         enter the Python venv (`$env:EDEV_VENV)"
    Write-Host "  westz <args…>         run 'west' from inside the Zephyr workspace"
    Write-Host "  edev_create_app <n>   scaffold a new Zephyr app at `$PWD\<n>\ (override: arg 2)"
    Write-Host "  edev_build [src] [b]  pristine sysbuild build (MCUboot + app)"
    Write-Host "  edev_build_simple     pristine non-sysbuild build"
    Write-Host "  edev_flash [src] [b]  flash via probe-rs"
    Write-Host "  edev_help             this message"
    Write-Host ""
    Write-Host "Defaults:"
    Write-Host "  EDEV_BOARD_DEFAULT = $env:EDEV_BOARD_DEFAULT"
    Write-Host "  EDEV_UF2_VOLUME    = $env:EDEV_UF2_VOLUME"
}
