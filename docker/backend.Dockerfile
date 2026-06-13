# Nabla AI — backend (orchestration) image.
#
# Multi-stage: stage 1 compiles the solver core, stage 2 is the Python
# runtime with the solver binary baked in at /usr/local/bin/nabla_solve.
# The solver stays behind the backend API (trade-secret boundary): this is
# the only container in the stack that contains or can execute it.
#
# Build from the repo root:
#   docker build -f docker/backend.Dockerfile \
#     --build-arg NABLA_GIT_SHA=$(git rev-parse --short=12 HEAD) -t nabla-backend .

# --- stage 1: solver core (keep in lockstep with docker/core.Dockerfile) -----
FROM debian:bookworm-slim AS core-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc g++ cmake make libhdf5-dev \
    && rm -rf /var/lib/apt/lists/*
ARG NABLA_GIT_SHA=unknown
WORKDIR /src
COPY core/ core/
RUN cmake -S core -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DNABLA_BUILD_TESTS=OFF \
        -DNABLA_GIT_SHA="${NABLA_GIT_SHA}" \
    && cmake --build build -j"$(nproc)" \
    && strip build/nabla_solve

# --- stage 2: FastAPI runtime -------------------------------------------------
FROM python:3.12-slim-bookworm
# libhdf5: solver checkpoint I/O.  curl: compose healthcheck.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libhdf5-103-1 curl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /usr/local/bin/

WORKDIR /app/backend
# Dependency layer first (cached until the lockfile changes).
COPY backend/pyproject.toml backend/uv.lock backend/README.md ./
RUN uv sync --frozen --no-dev --no-install-project
COPY backend/app ./app
RUN uv sync --frozen --no-dev
# The validation harness powers POST /api/runs/{id}/report (stdlib-only).
COPY validation /app/validation

COPY --from=core-build /src/build/nabla_solve /usr/local/bin/nabla_solve

ARG NABLA_GIT_SHA=unknown
ENV NABLA_SOLVER_COMMAND=/usr/local/bin/nabla_solve \
    NABLA_DATA_DIR=/data \
    NABLA_REPO_ROOT=/app \
    NABLA_GIT_SHA=${NABLA_GIT_SHA} \
    PATH="/app/backend/.venv/bin:${PATH}"

RUN useradd --system --home-dir /app --shell /usr/sbin/nologin nabla \
    && mkdir -p /data \
    && chown -R nabla:nabla /data /app
USER nabla
VOLUME ["/data"]

EXPOSE 8000
# --ws wsproto: the default websockets impl 400-rejects handshakes with >8 KiB
# header lines (big localhost cookie jars); wsproto has no such limit.
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--ws", "wsproto"]
