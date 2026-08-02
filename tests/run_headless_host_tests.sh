#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
DEFAULT_BINARY="$REPO_ROOT/../ProjectIgnis/ygopro"
HEADLESS_HOST_BINARY="${HEADLESS_HOST_BINARY:-$DEFAULT_BINARY}"

if [[ ! -x "$HEADLESS_HOST_BINARY" ]]; then
    echo "Missing executable headless host binary: $HEADLESS_HOST_BINARY" >&2
    echo "Set HEADLESS_HOST_BINARY to a built ygopro path." >&2
    exit 1
fi

BINARY_DIR=$(cd "$(dirname "$HEADLESS_HOST_BINARY")" && pwd)
TMP_DIR=$(mktemp -d)
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

find_free_port() {
    python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

assert_invalid_port_contract() {
    set +e
    (
        cd "$BINARY_DIR"
        "$HEADLESS_HOST_BINARY" --host-headless "Name=T032Invalid Port=0 Mode=single" \
            >"$TMP_DIR/invalid.stdout" 2>"$TMP_DIR/invalid.stderr"
    )
    local rc=$?
    set -e

    if [[ $rc -eq 0 ]]; then
        echo "Expected invalid port invocation to fail" >&2
        return 1
    fi

    python3 - "$TMP_DIR/invalid.stdout" <<'PY'
import json
import pathlib
import sys

lines = [line.strip() for line in pathlib.Path(sys.argv[1]).read_text().splitlines() if line.strip()]
if len(lines) != 1:
    raise SystemExit(f"Expected exactly one lifecycle event, got {len(lines)}")
event = json.loads(lines[0])
if event.get("event") != "ERROR":
    raise SystemExit(f"Expected ERROR event, got {event.get('event')!r}")
reason = event.get("detail", {}).get("reason", "")
if "1-65535" not in reason:
    raise SystemExit(f"Expected range error detail, got: {reason!r}")
PY
}

assert_start_and_stop_lifecycle() {
    local port
    port=$(find_free_port)

    (
        cd "$BINARY_DIR"
        "$HEADLESS_HOST_BINARY" --host-headless "Name=T032Room Port=$port Mode=single" \
            >"$TMP_DIR/lifecycle.stdout" 2>"$TMP_DIR/lifecycle.stderr"
    ) &
    local host_pid=$!

    if ! python3 - "$TMP_DIR/lifecycle.stdout" "$host_pid" <<'PY'
import pathlib
import sys
import time

out_path = pathlib.Path(sys.argv[1])
pid = int(sys.argv[2])
deadline = time.time() + 8.0

while time.time() < deadline:
    data = out_path.read_text() if out_path.exists() else ""
    if '"event":"ROOM_STARTED"' in data:
        raise SystemExit(0)
    try:
        pathlib.Path(f"/proc/{pid}").stat()
    except FileNotFoundError:
        raise SystemExit(1)
    time.sleep(0.1)

raise SystemExit(1)
PY
    then
        kill -TERM "$host_pid" >/dev/null 2>&1 || true
        wait "$host_pid" >/dev/null 2>&1 || true
        echo "Headless host did not emit ROOM_STARTED before timeout" >&2
        return 1
    fi

    kill -TERM "$host_pid" >/dev/null 2>&1 || true

    python3 - "$TMP_DIR/lifecycle.stdout" "$host_pid" <<'PY'
import pathlib
import sys
import time

out_path = pathlib.Path(sys.argv[1])
pid = int(sys.argv[2])
deadline = time.time() + 5.0

while time.time() < deadline:
    data = out_path.read_text() if out_path.exists() else ""
    if '"event":"ROOM_CLOSED"' in data:
        raise SystemExit(0)
    if not pathlib.Path(f"/proc/{pid}").exists():
        break
    time.sleep(0.1)

raise SystemExit(0)
PY

    wait "$host_pid" || true

    python3 - "$TMP_DIR/lifecycle.stdout" <<'PY'
import json
import pathlib
import sys

events = []
for raw in pathlib.Path(sys.argv[1]).read_text().splitlines():
    raw = raw.strip()
    if not raw:
        continue
    events.append(json.loads(raw))

if not events:
    raise SystemExit("Expected lifecycle events but got none")

names = [entry.get("event") for entry in events]
if "ROOM_STARTED" not in names:
    raise SystemExit("Missing ROOM_STARTED event")
if "ERROR" in names:
    raise SystemExit("Unexpected ERROR event during start/stop lifecycle check")

room_closed = next((entry for entry in events if entry.get("event") == "ROOM_CLOSED"), None)
if room_closed is not None:
    reason = room_closed.get("detail", {}).get("reason")
    if reason != "stopped":
        raise SystemExit(f"Expected ROOM_CLOSED reason stopped, got {reason!r}")
PY
}

assert_invalid_port_contract
assert_start_and_stop_lifecycle

echo "headless-host tests: PASS"