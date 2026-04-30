# =============================================================================
# edevkit — one-command venv setup (PowerShell)
# =============================================================================
# Mirror of tools/setup_venv.sh for Windows native PowerShell.
#
# Usage:
#   $env:EDEVKIT_REPO           = "..."
#   $env:EDEV_VENV              = "..."
#   $env:ZEPHYR_WORKSPACE       = "..."
#   $env:ZEPHYR_SDK_INSTALL_DIR = "..."
#   & "$env:EDEVKIT_REPO\tools\setup_venv.ps1"
# =============================================================================

$ErrorActionPreference = "Stop"

# 0. validate layout env vars
$missing = @()
if (-not $env:EDEVKIT_REPO)            { $missing += "EDEVKIT_REPO" }
if (-not $env:EDEV_VENV)               { $missing += "EDEV_VENV" }
if (-not $env:ZEPHYR_WORKSPACE)        { $missing += "ZEPHYR_WORKSPACE" }
if (-not $env:ZEPHYR_SDK_INSTALL_DIR)  { $missing += "ZEPHYR_SDK_INSTALL_DIR" }
if ($missing.Count -gt 0) {
    Write-Host "❌ missing layout env vars: $($missing -join ', ')" -ForegroundColor Red
    Write-Host "   See $env:EDEVKIT_REPO\tools\web\pages\installation.html → 'Pick a layout'."
    exit 1
}

Write-Host "📦 edevkit venv setup" -ForegroundColor Cyan
Write-Host "   EDEVKIT_REPO     = $env:EDEVKIT_REPO"
Write-Host "   EDEV_VENV        = $env:EDEV_VENV"
Write-Host "   ZEPHYR_WORKSPACE = $env:ZEPHYR_WORKSPACE"
Write-Host "   ZEPHYR_SDK       = $env:ZEPHYR_SDK_INSTALL_DIR"
Write-Host ""

# 1. create venv if not present
$activate = "$env:EDEV_VENV\Scripts\Activate.ps1"
if (Test-Path $activate) {
    Write-Host "✓ venv already exists at $env:EDEV_VENV — reusing" -ForegroundColor Green
} else {
    Write-Host "▸ creating venv at $env:EDEV_VENV"
    New-Item -ItemType Directory -Force -Path (Split-Path $env:EDEV_VENV) | Out-Null
    python -m venv $env:EDEV_VENV
}

# 2. install Python deps
Write-Host "▸ upgrading pip and installing west + docs-hub deps"
& "$env:EDEV_VENV\Scripts\python.exe" -m pip install --quiet --upgrade pip wheel
& "$env:EDEV_VENV\Scripts\pip.exe" install --quiet west
$req = "$env:EDEVKIT_REPO\tools\web\requirements.txt"
if (Test-Path $req) {
    & "$env:EDEV_VENV\Scripts\pip.exe" install --quiet -r $req
}

# 3. append helper-source line to Activate.ps1 (idempotent)
if ((Get-Content $activate) -match 'env_helpers\.ps1') {
    Write-Host "✓ Activate.ps1 already sources env_helpers.ps1 — skipping append"
} else {
    Write-Host "▸ appending helper-source line to $activate"
    Add-Content -Path $activate -Value @'

# --- edevkit helpers (auto-appended by tools/setup_venv.ps1) ---
# Pattern C: helpers live in the repo at $env:EDEVKIT_REPO\tools\env_helpers.ps1
# and get sourced when this venv is activated. Source-of-truth stays in git.
if ($env:EDEVKIT_REPO -and (Test-Path "$env:EDEVKIT_REPO\tools\env_helpers.ps1")) {
    . "$env:EDEVKIT_REPO\tools\env_helpers.ps1"
}
# --- end edevkit helpers ---
'@
}

# 4. west zephyr-export (if workspace exists)
if (Test-Path "$env:ZEPHYR_WORKSPACE\.west") {
    Write-Host "▸ running west zephyr-export against $env:ZEPHYR_WORKSPACE"
    Push-Location $env:ZEPHYR_WORKSPACE
    try {
        & "$env:EDEV_VENV\Scripts\west.exe" zephyr-export 2>&1 | Out-Null
    } catch { }
    finally { Pop-Location }
} else {
    Write-Host "ℹ Zephyr workspace not initialised at $env:ZEPHYR_WORKSPACE — skip 'west zephyr-export'"
    Write-Host "  (run installation.html step 4 once, then re-run this script)"
}

Write-Host ""
Write-Host "✅ venv ready." -ForegroundColor Green
Write-Host ""
Write-Host "   Activate now with:"
Write-Host '     . "$env:EDEV_VENV\Scripts\Activate.ps1"'
Write-Host ""
Write-Host "   After activation, the edev_* helpers are loaded automatically."
Write-Host "   Type 'edev_help' to see them."
