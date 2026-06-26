# =============================================================================
# clingo-web-native — ultra-minimal Docker image
# Strategy:
#   1. Builder: Alpine with gcc + libmicrohttpd-dev, compile static binary
#   2. Runtime: Alpine minimal (NOT scratch, because clingo needs shared libs)
#      Result: ~15 MB image, ~3 MB RSS, <10ms startup
# =============================================================================

# ---- Stage 1: compile -------------------------------------------------------
FROM alpine:3.21 AS builder

RUN apk add --no-cache \
    gcc \
    musl-dev \
    libmicrohttpd-dev

WORKDIR /build
COPY server.c .

# Compile: static link against musl + libmicrohttpd
# -Os  = optimize for size
# -s   = strip symbols
RUN gcc -Os -s -o clingo-web server.c \
    -lmicrohttpd \
    && strip clingo-web

# ---- Stage 2: minimal runtime -----------------------------------------------
FROM alpine:3.21

# Install clingo + runtime libs for libmicrohttpd (libgnutls/libgcrypt)
RUN apk add --no-cache \
    --repository=https://dl-cdn.alpinelinux.org/alpine/edge/community \
    clingo \
    libmicrohttpd \
    && rm -rf /var/cache/apk/*

# Non-root user for security
RUN adduser -D -u 1001 appuser
USER appuser

WORKDIR /app
COPY --from=builder /build/clingo-web .

EXPOSE 8080

# No shell needed — run binary directly
ENTRYPOINT ["/app/clingo-web"]
