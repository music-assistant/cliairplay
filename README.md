# cliairplay
Command line interface for audio streaming to AirPlay 2 devices

Based on [owntones](https://github.com/owntone/owntone-server) (all rights reserved).

## Debian build
NOTE: This is in early stage development and will be subject to change.
Install required tools and libraries - minimal list yet to be confirmed:
```
sudo apt-get install \
  build-essential git autotools-dev autoconf automake libtool gettext gawk \
  libconfuse-dev libunistring-dev \
  libavcodec-dev libavformat-dev libavfilter-dev libswscale-dev libavutil-dev \
  libasound2-dev libxml2-dev libgcrypt20-dev zlib1g-dev \
  libevent-dev libplist-dev libsodium-dev libjson-c-dev \
  libcurl4-openssl-dev libprotobuf-c-dev
```

Then run the following:
```
git clone https://github.com/music-assistant/cliairplay.git
cd cliairplay
git submodule update --init
autoreconf -i
./configure
make
```
