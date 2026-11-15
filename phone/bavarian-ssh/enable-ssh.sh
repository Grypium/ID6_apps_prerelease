#!/data/data/com.termux/files/usr/bin/bash
# Enable temporary SSH on the head unit from the phone, with no visible window.
# Runs the official Bavarian SSH Client (x86-64 Linux build) under emulation in
# the "deb64" proot container, on a hidden display, and presses Enable SSH.
#
# Usage: enable-ssh.sh [CAR_IP]      (leave CAR_IP out to let the client find it)
# Optional: TIMEOUT=240 enable-ssh.sh ...   (seconds to wait for a result)
set -euo pipefail

CAR_IP="${1:-}"
TIMEOUT="${TIMEOUT:-240}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOTFS="$PREFIX/var/lib/proot-distro/containers/deb64/rootfs"
SHOT="$HERE/last-screen.png"

if [[ -n "$CAR_IP" && ! "$CAR_IP" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
  echo "Car IP must look like 169.254.x.x" >&2; exit 2
fi
[[ -d "$ROOTFS" ]] || { echo "Container deb64 is missing. Run: proot-distro install -a x86_64 -n deb64 debian:12" >&2; exit 1; }
install -m 755 "$HERE/bavarian-ssh-client" "$ROOTFS/root/bavarian-ssh-client"

cat > "$ROOTFS/root/run-enable.sh" <<'INNER'
set -u
CAR_IP="$1"; TIMEOUT="$2"
export DISPLAY=:7 XDG_RUNTIME_DIR=/tmp/xdg ICED_BACKEND=tiny-skia
mkdir -p -m 700 /tmp/xdg
Xvfb :7 -screen 0 800x600x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
XVFB=$!
trap 'kill $CLIENT $XVFB 2>/dev/null' EXIT
sleep 3
cd /root && ./bavarian-ssh-client >/tmp/client.log 2>&1 &
CLIENT=$!

echo "Starting client (emulated; this takes about a minute)..."
WIN=""
for _ in $(seq 1 60); do
  WIN=$(xdotool search --name bavarian-ssh-client 2>/dev/null | head -1) && [ -n "$WIN" ] && break
  kill -0 $CLIENT 2>/dev/null || { echo "Client exited:"; tail -5 /tmp/client.log; exit 1; }
  sleep 2
done
[ -n "$WIN" ] || { echo "Client window never appeared."; exit 1; }
sleep 15   # let the first frame render under emulation

if [ -n "$CAR_IP" ]; then
  xdotool windowfocus --sync "$WIN" 2>/dev/null || xdotool windowfocus "$WIN"
  xdotool mousemove --window "$WIN" 260 27 click 1; sleep 3
  xdotool type --window "$WIN" --delay 250 "$CAR_IP"; sleep 3
  import -window root /root/typed.png 2>/dev/null
  typed=$(import -window root -crop 490x26+14+14 png:- 2>/dev/null | tesseract stdin - 2>/dev/null | tr -d ' \n')
  echo "IP box now shows: ${typed:-<empty>}"
fi
echo "Pressing Enable SSH${CAR_IP:+ for $CAR_IP}..."
xdotool mousemove --window "$WIN" 571 27 click 1

end=$((SECONDS + TIMEOUT)); last=""
while [ $SECONDS -lt $end ]; do
  sleep 8
  import -window root -crop 616x460+12+52 /root/status.png 2>/dev/null || continue
  text=$(tesseract /root/status.png - 2>/dev/null | grep -v '^\s*$' | grep -viE 'free software|bundling it|not permitted|scammed|leave it blank' || true)
  if [ "$text" != "$last" ]; then
    printf '%s\n' "$text" | sed "s/^/  | /" | tail -n +$(( $(printf '%s\n' "$last" | grep -c .) + 1 ))
    last="$text"
  fi
  if printf '%s' "$text" | grep -qi 'SSH is enabled'; then
    import -window root /root/last-screen.png; echo "RESULT: SSH is enabled."; exit 0
  fi
  kill -0 $CLIENT 2>/dev/null || break
done
import -window root /root/last-screen.png 2>/dev/null
echo "RESULT: no 'SSH is enabled' message within ${TIMEOUT}s. Check the screenshot."
exit 3
INNER

set +e
proot-distro login deb64 -- bash /root/run-enable.sh "$CAR_IP" "$TIMEOUT"
rc=$?
set -e
cp -f "$ROOTFS/root/last-screen.png" "$SHOT" 2>/dev/null && echo "Screenshot of the client: $SHOT"
exit $rc
