# ════════════════════════════════════════════════════════════════════
#  ChatBantu — Multi-stage Dockerfile for Render
#
#  WHY MULTI-STAGE:
#  Previously the Bantu binary was built on Debian 13 (glibc 2.41, GCC 14)
#  and copied into an Ubuntu 22.04 image. Even after stubbing high-version
#  symbols, the binary segfaulted on Render's Ubuntu 22.04 due to deeper
#  ABI differences (TLS init, libstdc++ internal layout, ifunc resolvers).
#
#  Building the binary INSIDE Ubuntu 22.04 guarantees 100% ABI compatibility
#  with the runtime image, because the binary is linked against the exact
#  same glibc / libstdc++ / libcurl that it will run against.
# ════════════════════════════════════════════════════════════════════

# ─── Stage 1: Builder ──────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Build tools + dev headers for SQLite and libcurl.
#   build-essential         → g++, gcc, make, libc-dev
#   binutils                → ld, as, objdump (explicit; transitively pulled
#                            by build-essential but listed here for clarity
#                            so build.sh's diagnostics never fail)
#   file                    → so we can inspect the binary type if linking fails
#   libsqlite3-dev          → sqlite3.h + libsqlite3.so (dev symlink)
#   libcurl4-openssl-dev    → curl/curl.h + libcurl.so (OpenSSL flavor,
#                            matches the runtime libcurl4 package)
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        gcc \
        make \
        binutils \
        file \
        libsqlite3-dev \
        libcurl4-openssl-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy the Bantu interpreter source tree
COPY bantu-src/compiler/ /build/compiler/

# Show what we have, so the Render log clearly shows tool versions.
RUN echo "=== Builder environment ===" \
    && uname -a \
    && cat /etc/os-release | head -3 \
    && g++ --version | head -1 \
    && gcc --version | head -1 \
    && ld --version | head -1 \
    && echo "libcurl dev:" && dpkg -l | grep -E 'libcurl4-openssl-dev|libsqlite3-dev' || true

# Build Bantu inside Ubuntu 22.04 — guaranteed ABI compatibility.
# We split "build" and "verify" into separate RUN layers so that, if build.sh
# fails, Render's log clearly shows the build.sh output without truncation
# (a single combined RUN would only show the exit code).
RUN cd /build/compiler \
    && chmod +x build.sh \
    && ./build.sh

# Verify the binary exists and is a Linux executable.
RUN test -f /build/compiler/build/bantu \
    && file /build/compiler/build/bantu \
    && cp /build/compiler/build/bantu /build/bantu \
    && chmod +x /build/bantu

# ─── Stage 2: Runtime ──────────────────────────────────────────────
FROM ubuntu:22.04

# Avoid tzdata / interactive prompts
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Africa/Dar_es_Salaam

# Runtime libraries the Bantu binary needs:
#   - libsqlite3-0  → libsqlite3.so.0  (SQLite)
#   - libcurl4      → libcurl.so.4     (Bantu's HTTP client, OpenSSL flavor)
#   - ca-certificates → TLS roots
#   - sqlite3       → optional CLI for DB inspection
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libsqlite3-0 \
        ca-certificates \
        sqlite3 \
        libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the freshly-built Bantu binary from the builder stage
COPY --from=builder /build/bantu /usr/local/bin/bantu
RUN chmod +x /usr/local/bin/bantu

# Copy the application (Bantu backend + static frontend)
COPY server.b /app/server.b
COPY public/  /app/public/

# Render mounts a persistent volume at /data for SQLite.
# Create it with world-writable perms so the bantu process can write
# regardless of which UID Render runs the container as.
RUN mkdir -p /data && chmod 777 /data

# Default port (Render injects $PORT)
ENV PORT=8080
EXPOSE 8080

# Pre-flight check: print glibc/libstdc++ requirements and verify the
# binary can actually start. If this fails, Render will show the error
# in the deploy logs instead of a cryptic runtime crash.
RUN echo "=== Bantu binary pre-flight ===" \
    && ldd /usr/local/bin/bantu \
    && /usr/local/bin/bantu --version

# Run the Bantu interpreter on server.b.
# Bantu's sua.server.listen($PORT) blocks forever, accepting HTTP.
CMD ["bantu", "run", "server.b"]
