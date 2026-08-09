# Multi-stage Dockerfile for C++ API Template

ARG APP_BINARY=cpp_api_template
ARG WORKER_BINARY=cpp_api_template_worker
ARG TEST_BINARY=cpp_api_template_tests_unit

# Stage 1: Build environment
FROM ubuntu:24.04 AS builder

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies (gcc-13/g++-13 are in noble's main, no PPA needed)
RUN apt-get update && apt-get install -y \
    git \
    pkg-config \
    libssl-dev \
    libsodium-dev \
    libcurl4-openssl-dev \
    zlib1g-dev \
    libjsoncpp-dev \
    uuid-dev \
    libpq-dev \
    libhiredis-dev \
    curl \
    wget \
    zip \
    unzip \
    tar \
    autoconf \
    automake \
    libtool \
    bison \
    flex \
    linux-libc-dev \
    gnupg \
    python3 \
    python3-pip \
    ninja-build \
    gcc-13 \
    g++-13 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100 \
    && rm -rf /var/lib/apt/lists/*

# Install CMake 3.20+ from Kitware APT repository
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null && \
    echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main' | tee /etc/apt/sources.list.d/kitware.list >/dev/null && \
    apt-get update && \
    apt-get install -y cmake && \
    rm -rf /var/lib/apt/lists/*

# Install vcpkg for dependency management.
# VCPKG_REF pins the vcpkg checkout to a known-good commit so the manifest
# baseline in /app/vcpkg.json resolves identically across machines.
# Override at build time only if you need a newer registry snapshot
# (you'll also want to bump builtin-baseline in vcpkg.json to match).
ARG VCPKG_REF=c4aad2868cfbe7c364192d5de82385647f8425b1
WORKDIR /opt
RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git && \
    cd vcpkg && \
    git checkout "$VCPKG_REF" && \
    ./bootstrap-vcpkg.sh

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"

# Manifest mode: copy only vcpkg.json so a source-only change doesn't
# bust this layer.
WORKDIR /app
COPY vcpkg.json ./

# Pre-build the whole dependency set in its own layer, BEFORE the sources
# arrive. This is the ~35-minute part of a cold build; keyed only on
# vcpkg.json it survives every source-only change — both in the local layer
# cache and via CI's gha/GHCR cache-from. --x-install-root matches the CMake
# toolchain default (${CMAKE_BINARY_DIR}/vcpkg_installed), so the configure
# step below finds everything installed and its manifest install is a no-op.
# BuildKit cache mounts keep vcpkg source tarballs and built-package archives
# across builds (and across FAILED builds — without them, one flaky download
# or compile error discards the whole dependency set with the RUN layer).
# The id= carries TARGETARCH: in a multi-arch buildx run both platform legs
# execute concurrently, and an UNSCOPED cache mount is shared between them —
# vcpkg then extracts tools (ninja) into the same path from both legs and one
# leg executes the other's binary (release v1.5.0 failed exactly this way).
ARG TARGETARCH
RUN --mount=type=cache,target=/opt/vcpkg/downloads,id=vcpkg-downloads-${TARGETARCH} \
    --mount=type=cache,target=/root/.cache/vcpkg,id=vcpkg-cache-${TARGETARCH} \
    vcpkg install --x-install-root=/app/build/vcpkg_installed

# Copy source code (after the dep layer, so editing src/ never rebuilds deps).
COPY . .

# Build application — the configure-time manifest install just validates the
# already-populated /app/build/vcpkg_installed from the layer above.
# ENABLE_WERROR=ON activates -Werror for the CI gate (see CMakeLists.txt).
# Pass --build-arg ENABLE_WERROR=ON from CI; defaults to OFF for local builds.
ARG ENABLE_WERROR=OFF

RUN --mount=type=cache,target=/opt/vcpkg/downloads,id=vcpkg-downloads-${TARGETARCH} \
    --mount=type=cache,target=/root/.cache/vcpkg,id=vcpkg-cache-${TARGETARCH} \
    mkdir -p build && cd build && \
    cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_WERROR=${ENABLE_WERROR} \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake && \
    cmake --build . --config Release -j$(nproc)

# Stage 2: Test runner (reuses builder, only runs tests)
FROM builder AS test-runner
ARG TEST_BINARY=cpp_api_template_tests_unit
ENV TEST_BINARY=${TEST_BINARY}

WORKDIR /app

# Create logs directory for observability tests
RUN mkdir -p logs

# Default env vars for Docker network (overridden by docker-compose)
ENV TEST_PG_HOST=localhost
ENV TEST_REDIS_HOST=127.0.0.1

ENTRYPOINT ["sh", "-c", "exec /app/build/$TEST_BINARY \"$@\"", "--"]
CMD ["--gtest_color=yes"]

# Stage 2b: Sanitizer test build (ASan + UBSan)
# Built as its own image layer so CI's test:sanitizers can `--cache-from` it and
# skip the recompile on an unchanged tree (registry-backed → shared across all
# runners; a host ccache can't survive true-dind jobs).
# VCPKG_INSTALLED_DIR reuses the deps the builder already installed under
# /app/build — without it a fresh `cmake -B build-san` would reinstall the whole
# dependency set from source (no binary cache inside the image).
FROM builder AS sanitizer-builder
# NOTE (council backlog, measured 2026-06): extending ASan to the integration
# bucket here is deferred until T1 Phase 2 (app_core). Compiling the integration
# TUs under ASan — heavy headers, no shared object file — OOM'd an 8 GB buildkit
# VM at ~24/39 (rpc EOF), and would risk the same on CI runners. Once the heavy
# bodies live in a single app_core .cpp, instrumenting them once is cheap; until
# then sanitizers cover the unit bucket only.
RUN cmake -B build-san -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_SANITIZERS=ON \
    -DVCPKG_INSTALLED_DIR=/app/build/vcpkg_installed \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake && \
    cmake --build build-san -j"$(nproc)" --target cpp_api_template_tests_unit

# Stage 3: Shared runtime base
FROM ubuntu:24.04 AS runtime-base

# Runtime dependencies (noble ships GCC 13 libstdc++ in main, no PPA needed).
# t64 transition renamed libssl3 → libssl3t64 and libhiredis0.14 → libhiredis1.1.0.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    libssl3t64 \
    libpq5 \
    libhiredis1.1.0 \
    libjsoncpp25 \
    libsodium23 \
    libcurl4t64 \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*
    # ca-certificates: --no-install-recommends skips it, and without the CA
    # bundle every outbound TLS from the app fails — first hit in prod was the
    # Mailer's SMTP STARTTLS to Brevo (curl: "Problem with the SSL CA cert").

# Create app user (noble's base image already has uid 1000 "ubuntu" — drop it first)
RUN userdel -r ubuntu 2>/dev/null || true && \
    useradd -m -u 1000 appuser && \
    mkdir -p /app/logs /app/config && \
    chown -R appuser:appuser /app

WORKDIR /app

# Stage 4: API runtime
FROM runtime-base AS runtime
ARG APP_BINARY=cpp_api_template
ENV APP_BINARY=${APP_BINARY}

COPY --from=builder --chown=appuser:appuser /app/build/${APP_BINARY} /app/
COPY --from=builder --chown=appuser:appuser /app/config /app/config/
COPY --from=builder --chown=appuser:appuser /app/migrations /app/migrations/
# Email templates are loaded at runtime from templates/email (cwd-relative).
COPY --from=builder --chown=appuser:appuser /app/templates /app/templates/

USER appuser

EXPOSE 8080 9090

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/healthz || exit 1

ENTRYPOINT ["sh", "-c", "exec /app/$APP_BINARY \"$@\"", "--"]
CMD ["config/config.json"]

# Stage 5: Worker runtime
FROM runtime-base AS worker-runtime
ARG WORKER_BINARY=cpp_api_template_worker
ENV WORKER_BINARY=${WORKER_BINARY}

COPY --from=builder --chown=appuser:appuser /app/build/${WORKER_BINARY} /app/
COPY --from=builder --chown=appuser:appuser /app/config /app/config/
COPY --from=builder --chown=appuser:appuser /app/migrations /app/migrations/
# Worker jobs can send mail too — same templates as the API runtime.
COPY --from=builder --chown=appuser:appuser /app/templates /app/templates/

USER appuser

EXPOSE 9091 9090

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:9091/healthz || exit 1

ENTRYPOINT ["sh", "-c", "exec /app/$WORKER_BINARY \"$@\"", "--"]
CMD ["config/config.json"]
