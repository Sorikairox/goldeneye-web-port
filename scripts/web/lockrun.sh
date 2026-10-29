#!/bin/bash
# Run a command while holding a cross-session lock file (PID inside), waiting while another live
# PID holds it. Used for /tmp/goldeneye-build.lock (any build/link/converter run in a container:
# two parallel Emscripten links crashed Docker Desktop twice) and /tmp/goldeneye-chrome.lock
# (one headless Chrome at a time).
#   scripts/web/lockrun.sh /tmp/goldeneye-build.lock scripts/web/build.sh
set -u
LOCK=$1; shift
while :; do
  if ( set -o noclobber; echo $$ > "$LOCK" ) 2>/dev/null; then break; fi
  holder=$(cat "$LOCK" 2>/dev/null || true)
  if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
    # stale lock (holder died): take it over only if it still names that dead PID
    if [ "$(cat "$LOCK" 2>/dev/null)" = "$holder" ]; then rm -f "$LOCK"; fi
    continue
  fi
  sleep 5
done
trap 'if [ "$(cat "$LOCK" 2>/dev/null)" = "$$" ]; then rm -f "$LOCK"; fi' EXIT INT TERM
"$@"
