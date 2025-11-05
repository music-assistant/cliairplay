# syntax=docker/dockerfile:1

# Docker image for building the cliap2 binary
ARG REPO
ARG BRANCH
ARG TARGETARCH

FROM debian:bookworm-slim AS cliap2-builder

ENV LANG C.UTF-8
WORKDIR /tmp

# Let's see if standard image has acceptable sources.list
RUN cat /etc/apt/sources.list

# And echo our input arguments
RUN echo REPO=$REPO \\
    && echo BRANCH=$BRANCH \\
    && echo TARGETARCH=$TARGETARCH \\
    && echo 1=$1 \\
    && echo 2=$2 \\
    && echo 3=$3 \\
    && ls -l /github/workspace

# Install build dependencies for cliap2
# RUN apt-get update && apt-get install -y --no-install-recommends \
#     build-essential \
#     git \
#     autotools-dev \
#     autoconf \
#     automake \
#     libtool \
#     gettext \
#     gawk gperf \
#     flex \
#     bison \
#     uuid-dev \
#     zlib1g-dev \
#     libcurl4-openssl-dev \
#     libsodium-dev \
#     libconfuse-dev \
#     libunistring-dev \
#     libxml2-dev \
#     libevent-dev \
#     libjson-c-dev \
#     libplist-dev \
#     libgcrypt20-dev \
#     libgpg-error-dev \
#     libavfilter-dev \
#     && rm -rf /var/lib/apt/lists/*

# RUN set -x \
#     && uname -a \
#     && cd /tmp \
#     && git clone $REPO \
#     && git fetch origin \
#     && get checkout $BRANCH \
#     && cd cliairplay \
#     && git submodule update --init \
#     && autoreconf -fi \
#     && ./configure \
#     && make \
#     && ls -l src/cliap2

# # Now need to work out how to commit the built binary back into $REPO's $BRANCH
# COPY src/cliap2 .

# CMD ["./cliap2"]
