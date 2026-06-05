# syntax=docker/dockerfile:1.7
#
# Asterisk Opus DevContainer - Debian Bookworm (glibc)
#
# Default devcontainer image. Alpine variant: Dockerfile.alpine
#

ARG DEBIAN_VERSION=bookworm
ARG ASTERISK_VERSION=23.1.0

# ------------------------------------------------------------
# BASE IMAGE PREPARATION
# ------------------------------------------------------------

FROM debian:${DEBIAN_VERSION}-slim AS base

ARG DEBIAN_VERSION
ARG ASTERISK_VERSION

LABEL maintainer="Yoann Vanitou <yoann@amical-ai.com>"
LABEL org.opencontainers.image.authors="Amical, Yoann Vanitou <yoann@amical-ai.com>"
LABEL org.opencontainers.image.source="https://github.com/amicalai/asterisk-opus"
LABEL org.opencontainers.image.description="Asterisk Opus codec development environment (Debian/glibc)"
LABEL org.opencontainers.image.licenses="MIT"
LABEL version="${ASTERISK_VERSION}"

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libasound2 \
    ffmpeg \
    freetds-common \
    libsybdb5 \
    libjansson4 \
    libcurl4 \
    libedit2 \
    libltdl7 \
    libogg0 \
    libsrtp2-1 \
    libuuid1 \
    libvorbis0a \
    libvorbisenc2 \
    libvorbisfile3 \
    libxml2 \
    lua5.2 \
    liblua5.2-0 \
    libncurses6 \
    libtinfo6 \
    libopus0 \
    opus-tools \
    libopusenc0 \
    libopusfile0 \
    libpopt0 \
    libportaudio2 \
    libspeex1 \
    libspeexdsp1 \
    libsqlite3-0 \
    vorbis-tools \
    zlib1g \
    build-essential \
    patch \
    git \
    bison \
    flex \
    curl \
    libcurl4-openssl-dev \
    tar \
    libsqlite3-dev \
    liblua5.2-dev \
    libssl-dev \
    libxml2-dev \
    libopus-dev \
    libspeex-dev \
    libspeexdsp-dev \
    freetds-dev \
    libvorbis-dev \
    libpopt-dev \
    portaudio19-dev \
    libdb-dev \
    uuid-dev \
    libsrtp2-dev \
    autoconf \
    automake \
    libjansson-dev \
    libtool \
    libnewt-dev \
    zlib1g-dev \
    xmlstarlet \
    libedit-dev \
    linux-libc-dev \
    libogg-dev \
    libltdl-dev \
    libasound2-dev \
    libopusenc-dev \
    libopusfile-dev \
    python3 \
    python3-dev \
    gdb \
    valgrind \
    libasan8 \
    && rm -rf /var/lib/apt/lists/*

# ------------------------------------------------------------
# BUILDER IMAGE PREPARATION
# ------------------------------------------------------------

FROM base AS builder

ARG DEBIAN_VERSION
ARG ASTERISK_VERSION

WORKDIR /usr/src

RUN --mount=type=cache,target=/root/.cache/git \
    git clone --depth 1 --branch ${ASTERISK_VERSION} https://github.com/asterisk/asterisk.git asterisk

COPY include/asterisk/ asterisk/include/asterisk/
COPY codecs/ asterisk/codecs/
COPY res/ asterisk/res/

WORKDIR /usr/src/asterisk

RUN ./bootstrap.sh && \
    CFLAGS="-ggdb -O0 -fno-omit-frame-pointer -fstack-protector-all" \
    ./configure \
    --prefix=/usr \
    --sysconfdir=/etc \
    --mandir=/usr/share/man \
    --libdir=/usr/lib \
    --localstatedir=/var \
    --disable-xmldoc \
    --enable-permanent-dlopen \
    --enable-dev-mode \
    --with-gsm=internal \
    --with-popt \
    --with-z \
    --with-newt \
    --with-sqlite3 \
    --with-speex \
    --without-dahdi \
    --without-pri \
    --without-tonezone \
    --without-x11 \
    --with-pjproject-bundled \
    --with-libcurl \
    --with-libedit \
    --with-srtp \
    --with-opus

RUN make menuselect.makeopts && \
    menuselect/menuselect \
    --disable BUILD_NATIVE \
    --disable DONT_OPTIMIZE \
    --disable CORE-SOUNDS-EN-GSM \
    --disable app_voicemail \
    --enable BETTER_BACKTRACES \
    --enable codec_opus_open_source \
    --enable CORE-SOUNDS-EN-WAV \
    --enable CORE-SOUNDS-FR-WAV \
    --enable res_format_attr_opus \
    --enable res_http_post \
    --enable res_http_websocket \
    --enable res_pjproject \
    --enable res_pjsip \
    --enable res_pjsip_session \
    --enable res_rtp_asterisk \
    --enable res_stasis \
    --enable app_stasis \
    --enable res_timing_timerfd \
    menuselect.makeopts

RUN make -j$(nproc) && \
    make install && \
    make basic-pbx

# ------------------------------------------------------------
# FINAL DEV IMAGE
# ------------------------------------------------------------

FROM mcr.microsoft.com/devcontainers/base:${DEBIAN_VERSION}

ARG DEBIAN_VERSION
ARG ASTERISK_VERSION

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libasound2 \
    ffmpeg \
    freetds-common \
    libsybdb5 \
    libjansson4 \
    libcurl4 \
    libedit2 \
    libltdl7 \
    libogg0 \
    libsrtp2-1 \
    libuuid1 \
    libvorbis0a \
    libvorbisenc2 \
    libvorbisfile3 \
    libxml2 \
    lua5.2 \
    liblua5.2-0 \
    libncurses6 \
    libtinfo6 \
    libopus0 \
    opus-tools \
    libopusenc0 \
    libopusfile0 \
    libpopt0 \
    libportaudio2 \
    libspeex1 \
    libspeexdsp1 \
    libsqlite3-0 \
    vorbis-tools \
    zlib1g \
    build-essential \
    patch \
    git \
    bison \
    flex \
    curl \
    libcurl4-openssl-dev \
    tar \
    libsqlite3-dev \
    liblua5.2-dev \
    libssl-dev \
    libxml2-dev \
    libopus-dev \
    libspeex-dev \
    libspeexdsp-dev \
    freetds-dev \
    libvorbis-dev \
    libpopt-dev \
    portaudio19-dev \
    libdb-dev \
    uuid-dev \
    libsrtp2-dev \
    autoconf \
    automake \
    libjansson-dev \
    libtool \
    libnewt-dev \
    zlib1g-dev \
    xmlstarlet \
    libedit-dev \
    linux-libc-dev \
    libogg-dev \
    libltdl-dev \
    libasound2-dev \
    libopusenc-dev \
    libopusfile-dev \
    gdb \
    valgrind \
    libasan8 \
    && rm -rf /var/lib/apt/lists/*

COPY --link --chmod=755 --from=builder /usr/sbin/asterisk /usr/sbin/
COPY --link --chmod=755 --from=builder /usr/sbin/astcanary /usr/sbin/
COPY --link --from=builder /usr/lib/asterisk /usr/lib/asterisk
COPY --link --from=builder /usr/lib/libasterisk* /usr/lib/
COPY --link --chmod=644 --from=builder /etc/asterisk /etc/asterisk
COPY --link --chown=vscode:vscode --chmod=755 --from=builder /var/spool/asterisk /var/spool/asterisk
COPY --link --chown=vscode:vscode --chmod=755 --from=builder /var/run/asterisk /var/run/asterisk
COPY --link --chown=vscode:vscode --chmod=755 --from=builder /var/lib/asterisk /var/lib/asterisk
COPY --link --from=builder /usr/src/asterisk /usr/src/asterisk

RUN mkdir -p /var/log/asterisk && \
    chown -R vscode:vscode /var/log/asterisk && \
    chmod 755 /var/log/asterisk

RUN LUA_LIB="$(find /usr/lib -name 'liblua5.2.so.0*' | head -1)" && \
    ln -sf "$LUA_LIB" /usr/lib/liblua.so && \
    ln -sf "$LUA_LIB" /usr/lib/liblua.so.5

WORKDIR /workspaces/asterisk-opus
