#!/usr/bin/env bash
# Apply the vendored Zephyr patches in this directory to a Zephyr tree.
#
# Usage:
#   ./apply.sh                              # uses $ZEPHYR_BASE
#   ./apply.sh /path/to/zephyrproject/zephyr
#
# Idempotent — a patch already applied is skipped, not re-applied. Bails
# without touching anything if a patch can't be applied cleanly (e.g.
# local conflicts in drivers/dp/). See README.md for context.

set -euo pipefail

PATCHES_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ZEPHYR=${1:-${ZEPHYR_BASE:-}}

if [[ -z "$ZEPHYR" ]]; then
	echo "error: no Zephyr path. Set ZEPHYR_BASE or pass it as \$1." >&2
	exit 2
fi
if [[ ! -f "$ZEPHYR/Kconfig.zephyr" ]]; then
	echo "error: $ZEPHYR doesn't look like a Zephyr tree (no Kconfig.zephyr)" >&2
	exit 2
fi

cd "$ZEPHYR"
applied=0 skipped=0

for patch in "$PATCHES_DIR"/*.patch; do
	name=$(basename "$patch")

	# Already-applied check: -R --check succeeds iff every hunk in the
	# patch is already present (reverse-apply would succeed).
	if git apply -R --check "$patch" 2>/dev/null; then
		echo "[skip] $name (already applied)"
		skipped=$((skipped + 1))
		continue
	fi

	# Will-apply check: --check succeeds iff every hunk applies cleanly.
	if ! git apply --check "$patch" 2>/dev/null; then
		echo "[fail] $name — refuses to apply cleanly." >&2
		echo "       Check for local changes in drivers/dp/, or rebase" >&2
		echo "       onto a clean Zephyr checkout before running apply.sh." >&2
		git apply --check "$patch" 2>&1 | sed 's/^/         /' >&2 || true
		exit 1
	fi

	git apply "$patch"
	echo "[ok]   $name"
	applied=$((applied + 1))
done

echo
echo "Done — $applied applied, $skipped already in place."
