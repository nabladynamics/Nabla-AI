#!/usr/bin/env bash
# Nabla AI — local development entry point.
#
# Builds the solver core in Release mode, then starts the backend (FastAPI,
# :8000) and the frontend (Vite, :5173). Everything runs locally. Ctrl-C stops
# both servers.
#
# Env overrides: BACKEND_PORT (8000), FRONTEND_PORT (5173).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Pick the first free port at/above the preferred one (Docker or another dev
# server may already hold the default — common on machines running the demo).
# Checks BOTH loopback families: vite/node servers often bind only [::1].
port_busy() {
  python3 - "$1" <<'PY'
import socket, sys
port = int(sys.argv[1])
for family, addr in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1")):
    s = socket.socket(family, socket.SOCK_STREAM)
    s.settimeout(0.25)
    try:
        if s.connect_ex((addr, port)) == 0:
            sys.exit(0)  # something is listening -> busy
    except OSError:
        pass
    finally:
        s.close()
sys.exit(1)
PY
}
pick_port() { local p="$1"; while port_busy "${p}"; do p=$((p + 1)); done; echo "${p}"; }
BACKEND_PORT="${BACKEND_PORT:-$(pick_port 8000)}"
FRONTEND_PORT="${FRONTEND_PORT:-$(pick_port 5173)}"
# the Vite proxy reads BACKEND_PORT to follow the backend (vite.config.ts)
export BACKEND_PORT

log() { printf '\033[1;34m[dev]\033[0m %s\n' "$*"; }

pids=()
cleanup() {
  log "shutting down …"
  for pid in "${pids[@]:-}"; do
    [[ -n "${pid:-}" ]] || continue
    # Kill the whole process group started by each background job.
    kill -- "-${pid}" 2>/dev/null || kill "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

# Each background job runs in its own process group (setsid via `set -m`) so we
# can take its children down with it on shutdown.
set -m

# 1) Build the core (Release).
"${ROOT}/scripts/build-core.sh"

# 2) Backend — FastAPI on :8000
log "starting backend → http://localhost:${BACKEND_PORT}/docs"
(
  cd "${ROOT}/backend"
  uv sync --quiet
  # --ws wsproto: the default websockets impl rejects handshakes whose Cookie
  # header exceeds 8 KiB with a bare 400 — common on dev machines where every
  # localhost app shares the cookie jar. wsproto has no such per-line limit.
  exec uv run uvicorn app.main:app --reload --ws wsproto --port "${BACKEND_PORT}"
) &
pids+=("$!")

# 3) Frontend — Vite on :5173
log "starting frontend → http://localhost:${FRONTEND_PORT}"
(
  cd "${ROOT}/frontend"
  [[ -d node_modules ]] || npm install
  # raise node's 16 KiB total-header cap for the same big-cookie reason
  export NODE_OPTIONS="--max-http-header-size=65536 ${NODE_OPTIONS:-}"
  exec npm run dev -- --port "${FRONTEND_PORT}" --strictPort
) &
pids+=("$!")

log "both servers starting; press Ctrl-C to stop."
wait
