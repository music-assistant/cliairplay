# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS cliap2-builder

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
    autoconf-archive \
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
    libavfilter-dev

COPY . /tmp

WORKDIR /tmp

RUN set -x \
    && uname -a \
    && pwd \
    && echo ACLOCAL_PATH=$ACLOCAL_PATH \
    && export ACLOCAL_PATH=/usr/share/aclocal:./m4 \
    && echo ACLOCAL_PATH=$ACLOCAL_PATH \
    && ls -la \
    && ls -la m4 \
    && litoolize --force --copy --automake \
    && aclocal \
    && autoheader \
    && automake --add-missing --copy --force-missing \
    && autoconf
    && ls -la m4 \
    && ls -la build-aux \
    && ./configure \
    && make \
    && ls -l src/cliap2 \
    && cp -v src/cliap2 ./cliap2-$TARGETARCH

CMD ["./cliap2-$TARGETARCH"]
