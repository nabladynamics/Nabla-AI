#!/usr/bin/env bash
# Nabla AI — one-command investor demo.
#
#   scripts/demo.sh            build + start the stack, seed a cube run, open the browser
#   scripts/demo.sh down       stop the stack (run data persists in the nabla-data volume)
#
# Requires Docker (with compose v2). Ports default to 8080 (web) / 8000 (API)
# and fall back automatically if something else is already listening; override
# with NABLA_FRONTEND_PORT / NABLA_BACKEND_PORT (or a .env file).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

log() { printf '\033[1;35m[demo]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[demo]\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || fail "Docker is required: https://docs.docker.com/get-docker/"
docker compose version >/dev/null 2>&1 || fail "Docker Compose v2 is required (docker compose ...)"

if [[ "${1:-}" == "down" ]]; then
  docker compose down
  log "stack stopped. Run data persists in the 'nabla-data' volume."
  exit 0
fi

# --- ports: honor overrides, otherwise pick the first free port -------------
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
pick_port() { # $1 = preferred
  local p="$1"
  while port_busy "${p}"; do p=$((p + 1)); done
  echo "${p}"
}
export NABLA_FRONTEND_PORT="${NABLA_FRONTEND_PORT:-$(pick_port 8080)}"
export NABLA_BACKEND_PORT="${NABLA_BACKEND_PORT:-$(pick_port 8000)}"

# --- build provenance: stamp the images with the current commit -------------
export NABLA_GIT_SHA="${NABLA_GIT_SHA:-$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}"

log "building + starting the stack (web :${NABLA_FRONTEND_PORT}, api :${NABLA_BACKEND_PORT}, git ${NABLA_GIT_SHA}) …"
docker compose up -d --build

# --- wait for the backend to come up -----------------------------------------
API="http://localhost:${NABLA_BACKEND_PORT}"
for _ in $(seq 1 60); do
  curl -fsS "${API}/health" >/dev/null 2>&1 && break
  sleep 2
done
curl -fsS "${API}/health" >/dev/null || fail "backend did not become healthy; see: docker compose logs backend"
log "backend healthy: $(curl -fsS "${API}/health")"

# --- seed the demo run: wall-mounted cube at Re_h = 500 ----------------------
STL="core/examples/cube.stl"
[[ -f "${STL}" ]] || fail "bundled demo geometry missing: ${STL}"

log "seeding demo run (wall-mounted cube, Re_h=500, 240 steps) …"
CONFIG='{"type":"wall-mounted-cube","reynolds":500,"resolution":6,"max_steps":240,"snapshot_every":20,"checkpoint_every":80}'
RUN_ID=$(curl -fsS -X POST "${API}/api/runs" \
  -F "stl=@${STL};type=model/stl" \
  -F "config=${CONFIG}" \
  -F "name=demo-cube-re500" | python3 -c "import json,sys; print(json.load(sys.stdin)['run']['id'])")
curl -fsS -X POST "${API}/api/runs/${RUN_ID}/start" >/dev/null
log "run ${RUN_ID} started — watch it live in the Simulation tab."

# --- open the browser ---------------------------------------------------------
URL="http://localhost:${NABLA_FRONTEND_PORT}"
log "demo ready → ${URL}"
log "  Pre-simulation : upload geometry, ask the co-pilot, confirm the card"
log "  Simulation     : live charts + adaptive fidelity map for ${RUN_ID}"
log "  Post-simulation: flow field, vortex structures, spectra, report"
case "$(uname -s)" in
  Darwin) open "${URL}" ;;
  Linux) command -v xdg-open >/dev/null && xdg-open "${URL}" || true ;;
esac
