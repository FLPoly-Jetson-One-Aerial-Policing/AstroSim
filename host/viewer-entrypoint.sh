#!/bin/bash
# Starts a virtual display, VNC + noVNC (port 6080), then the viewer against the engine.
# ENGINE_HOST (default "engine") may be a hostname; the viewer only accepts IPv4 literals, so resolve it here.
set -eu
ENGINE_HOST=${ENGINE_HOST:-engine}
ENGINE_PORT=${ENGINE_PORT:-8765}
SIZE=${DISPLAY_SIZE:-1280x720}

ip=$ENGINE_HOST
if ! [[ $ip =~ ^[0-9.]+$ ]]; then
  for _ in $(seq 1 30); do
    ip=$(getent ahostsv4 "$ENGINE_HOST" | awk 'NR==1{print $1}') && [[ -n $ip ]] && break
    sleep 1
  done
  [[ -n $ip ]] || { echo "cannot resolve $ENGINE_HOST" >&2; exit 1; }
fi

Xvfb "$DISPLAY" -screen 0 "${SIZE}x24" +extension GLX +render -noreset &
for _ in $(seq 1 50); do [[ -e /tmp/.X11-unix/X${DISPLAY#:} ]] && break; sleep 0.1; done
x11vnc -display "$DISPLAY" -forever -shared -nopw -quiet -rfbport 5900 &
websockify --web /usr/share/novnc 6080 localhost:5900 &
trap 'kill $(jobs -p) 2>/dev/null' TERM INT EXIT

echo "viewer: open http://localhost:6080/vnc.html?autoconnect=1&resize=scale  (engine $ip:$ENGINE_PORT)"
viewer "$ip" "$ENGINE_PORT"
