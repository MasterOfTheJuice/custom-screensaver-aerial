#!/usr/bin/env bash
# deploy-and-check.sh
# Usage: ./deploy-and-check.sh
# Assumes: SSH key auth to TV, ffmpeg available, RTSP URL reachable.

set -euo pipefail

TV_HOST="${TV_HOST:-root@192.168.11.55}"
RTSP_URL="${RTSP_URL:-rtsp://192.168.11.179:554/live/ch00_0}"  # change me
FRAME_OUT="${FRAME_OUT:-/tmp/aerial-frame.png}"
IPK_PATTERN="${IPK_PATTERN:-*.ipk}"

echo "=== Building / locating IPK ==="
# Adjust this section to your actual build command.
# Example placeholder:
# npm run build && ares-package ...
# For now we just look for an existing IPK.
IPK=$(ls -t $IPK_PATTERN 2>/dev/null | head -1 || true)
if [[ -z "$IPK" ]]; then
  echo "No IPK found. Build one first."
  exit 1
fi
echo "Using IPK: $IPK"

echo "=== Deploying to TV ==="
scp "$IPK" "$TV_HOST:/tmp/"
ssh "$TV_HOST" "ares-install -d /tmp/$(basename "$IPK") || true; luna-send -n 1 luna://com.webos.service.tvpower/power/turnOnScreenSaver {}"

echo "=== Waiting for screensaver ==="
sleep 5

echo "=== Capturing RTSP frame ==="
ffmpeg -y -rtsp_transport tcp -i "$RTSP_URL" -frames:v 1 -q:v 2 "$FRAME_OUT" 2>/dev/null || {
  echo "ffmpeg failed to capture frame"
  exit 1
}

echo "=== Frame captured: $FRAME_OUT ==="
ls -lh "$FRAME_OUT"
echo "Agent should now inspect $FRAME_OUT and decide if the video is visible."
