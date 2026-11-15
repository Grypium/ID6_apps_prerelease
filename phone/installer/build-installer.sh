#!/data/data/com.termux/files/usr/bin/bash
# Build the official (old-main) installer for Android/Termux with Ethernet socket
# binding, embedding the official app payload from the release in git history.
# Usage: phone/installer/build-installer.sh [OUTPUT]   (default ~/nbt-installer-enet)
# Needs: git, go, clang (pkg install golang clang), python3.
set -euo pipefail
OUT="${1:-$HOME/nbt-installer-enet}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(git -C "$HERE" rev-parse --show-toplevel)"
INSTALLER_COMMIT=ade9c30abe37e8f5ddc5614a1e59ce1a1cae829c   # last upstream commit with the Go installer
RELEASE_COMMIT=f4ec31e                                       # official release zips (2026-11-07)
for c in $INSTALLER_COMMIT $RELEASE_COMMIT; do
  git -C "$REPO" cat-file -e "$c^{commit}" 2>/dev/null || {
    echo "Missing commit $c. Fetch the preserved upstream history first:"
    echo "  git fetch origin upstream-old-main"; exit 1; }
done
WORK="$(mktemp -d)"
trap 'git -C "$REPO" worktree remove --force "$WORK/src" 2>/dev/null; rm -rf "$WORK"' EXIT
git -C "$REPO" worktree add --detach "$WORK/src" $INSTALLER_COMMIT >/dev/null
git -C "$WORK/src" apply "$HERE/android-enet.patch"
git -C "$REPO" show $RELEASE_COMMIT:NBT-Evo-Gauges-Windows-x64.zip > "$WORK/win.zip"
mkdir "$WORK/rel" && (cd "$WORK/rel" && unzip -q ../win.zip)
python3 "$HERE/extract_payload.py" "$WORK/rel/NBT Evo Gauge Installer.exe" "$WORK/src/source" >/dev/null
(cd "$WORK/src/source" && python3 tools/stage_payload.py --check)
(cd "$WORK/src/source" && CGO_ENABLED=1 go build -o "$OUT" .)
echo "Built $OUT"
