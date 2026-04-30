#!/usr/bin/env bash
# =============================================================================
# edevkit — one-command venv setup (bash / zsh)
# =============================================================================
# Creates the Python venv at $EDEV_VENV, installs west + the docs-hub deps,
# and wires the helpers into the venv's activate script (Pattern C — the
# source-of-truth helpers in $EDEVKIT_REPO/tools/env_helpers.sh stay in git).
#
# Usage:
#   export EDEVKIT_REPO=...            # see installation.html → "Pick a layout"
#   export EDEV_VENV=...
#   export ZEPHYR_WORKSPACE=...
#   export ZEPHYR_SDK_INSTALL_DIR=...
#   bash $EDEVKIT_REPO/tools/setup_venv.sh
#
# Idempotent — safe to re-run; existing venv is reused, helper-source line
# is only appended once.
# =============================================================================

set -e

# ─── 0. validate layout env vars ─────────────────────────────────────────────
missing=""
[ -z "${EDEVKIT_REPO:-}" ]            && missing="$missing EDEVKIT_REPO"
[ -z "${EDEV_VENV:-}" ]               && missing="$missing EDEV_VENV"
[ -z "${ZEPHYR_WORKSPACE:-}" ]        && missing="$missing ZEPHYR_WORKSPACE"
[ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ]  && missing="$missing ZEPHYR_SDK_INSTALL_DIR"
if [ -n "$missing" ]; then
    echo "❌ missing layout env vars:$missing"
    echo "   Export them first — see $EDEVKIT_REPO/tools/web/pages/installation.html"
    echo "   section 'Pick a layout' for the three layout patterns."
    exit 1
fi

echo "📦 edevkit venv setup"
echo "   EDEVKIT_REPO     = $EDEVKIT_REPO"
echo "   EDEV_VENV        = $EDEV_VENV"
echo "   ZEPHYR_WORKSPACE = $ZEPHYR_WORKSPACE"
echo "   ZEPHYR_SDK       = $ZEPHYR_SDK_INSTALL_DIR"
echo ""

# ─── 1. create venv if it doesn't exist ──────────────────────────────────────
if [ -d "$EDEV_VENV" ] && [ -f "$EDEV_VENV/bin/activate" ]; then
    echo "✓ venv already exists at $EDEV_VENV — reusing"
else
    echo "▸ creating venv at $EDEV_VENV"
    mkdir -p "$(dirname "$EDEV_VENV")"
    python3 -m venv "$EDEV_VENV"
fi

# ─── 2. upgrade pip + install required Python deps ───────────────────────────
echo "▸ upgrading pip and installing west + docs-hub deps"
"$EDEV_VENV/bin/pip" install --quiet --upgrade pip wheel
"$EDEV_VENV/bin/pip" install --quiet west
if [ -f "$EDEVKIT_REPO/tools/web/requirements.txt" ]; then
    "$EDEV_VENV/bin/pip" install --quiet -r "$EDEVKIT_REPO/tools/web/requirements.txt"
fi

# ─── 3. append helper-source line to venv activate (idempotent) ──────────────
ACTIVATE="$EDEV_VENV/bin/activate"
if /usr/bin/grep -q 'env_helpers.sh' "$ACTIVATE"; then
    echo "✓ activate script already sources env_helpers.sh — skipping append"
else
    echo "▸ appending helper-source line to $ACTIVATE"
    cat >> "$ACTIVATE" <<'EOF'

# --- edevkit helpers (auto-appended by tools/setup_venv.sh) ---
# Pattern C: helpers live in the repo at $EDEVKIT_REPO/tools/env_helpers.sh
# and get sourced when this venv is activated. Source-of-truth stays in git.
if [ -n "${EDEVKIT_REPO:-}" ] && [ -f "$EDEVKIT_REPO/tools/env_helpers.sh" ]; then
    # shellcheck disable=SC1090
    source "$EDEVKIT_REPO/tools/env_helpers.sh"
fi
# --- end edevkit helpers ---
EOF
fi

# ─── 4. (optional) west zephyr-export against the existing workspace ─────────
if [ -d "$ZEPHYR_WORKSPACE/.west" ]; then
    echo "▸ running west zephyr-export against $ZEPHYR_WORKSPACE"
    ( cd "$ZEPHYR_WORKSPACE" && "$EDEV_VENV/bin/west" zephyr-export >/dev/null 2>&1 ) || true
else
    echo "ℹ Zephyr workspace not initialised at $ZEPHYR_WORKSPACE — skip 'west zephyr-export'"
    echo "  (run installation.html step 4 once, then re-run this script)"
fi

# ─── 5. report ───────────────────────────────────────────────────────────────
echo ""
echo "✅ venv ready."
echo ""
echo "   Activate now with:"
echo "     source \"\$EDEV_VENV/bin/activate\""
echo ""
echo "   After activation, the edev_* helpers are loaded automatically."
echo "   Type 'edev_help' to see them."
