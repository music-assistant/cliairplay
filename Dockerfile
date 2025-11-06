# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS cliap2-builder

ARG REPO
ARG BRANCH
ARG TARGETARCH

ENV LANG=C.UTF-8

RUN echo REPO=$REPO \
    && echo BRANCH=$BRANCH \
    && echo TARGETARCH=$TARGETARCH

# Create our Debian package sources list
RUN echo "deb http://deb.debian.org/debian bookworm main contrib non-free non-free-firmware" > /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian-security bookworm-security main contrib non-free non-free-firmware" >> /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian bookworm-updates main contrib non-free non-free-firmware" >> /etc/apt/sources.list

# Install build dependencies for cliap2
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    autotools-dev \
    autoconf \
    automake \
    libtool \
    gettext \
    gawk gperf \
    flex \
    bison \
    uuid-dev \
    zlib1g-dev \
    libcurl4-openssl-dev \
    libsodium-dev \
    libconfuse-dev \
    libunistring-dev \
    libxml2-dev \
    libevent-dev \
    libjson-c-dev \
    libplist-dev \
    libgcrypt20-dev \
    libgpg-error-dev \
    libavfilter-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /tmp

WORKDIR /tmp

RUN set -x \
    && uname -a \
    && pwd \
    && ls -la \
    && find . -name cliairplay -print

# RUN git clone $REPO \
#     && git fetch origin \
#     && get checkout $BRANCH \
#     && ls -laR

    # && autoreconf -fi \
    # && ./configure \
    # && make \
    # && ls -l src/cliap2

# # Now need to work out how to commit the built binary back into $REPO's $BRANCH
# COPY src/cliap2 .

# CMD ["./cliap2"]
