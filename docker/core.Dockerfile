# Nabla AI — solver core artifact image (multi-stage, slim runtime).
#
# This image carries ONLY the headless solver CLI. It is an internal build
# artifact (CI, on-prem delivery of the full stack): the solver is never
# distributed to clients as a raw binary — in the deployed stack it lives
# inside the backend container, behind the orchestration API (CLAUDE.md
# rule 4 / ADR-0006).
#
# Build from the repo root (the SHA comes in as a build arg because .git is
# not in the context):
#   docker build -f docker/core.Dockerfile \
#     --build-arg NABLA_GIT_SHA=$(git rev-parse --short=12 HEAD) -t nabla-core .

FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc g++ cmake make libhdf5-dev \
    && rm -rf /var/lib/apt/lists/*
ARG NABLA_GIT_SHA=unknown
WORKDIR /src
COPY core/ core/
# Keep this configure/build block in lockstep with docker/backend.Dockerfile.
RUN cmake -S core -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DNABLA_BUILD_TESTS=OFF \
        -DNABLA_GIT_SHA="${NABLA_GIT_SHA}" \
    && cmake --build build -j"$(nproc)" \
    && strip build/nabla_solve

FROM debian:bookworm-slim AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends libhdf5-103-1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/nabla_solve /usr/local/bin/nabla_solve
USER 65534:65534
ENTRYPOINT ["nabla_solve"]
CMD ["--help"]
