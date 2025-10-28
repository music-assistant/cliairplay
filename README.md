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

## macOS build

Below are instructions for preparing macOS (Intel or Apple Silicon) to build this project using Homebrew.

1. Install Xcode Command Line Tools (if not already installed):

```zsh
xcode-select --install
```

2. Install Homebrew (if you don't have it):

Visit https://brew.sh/ and follow the instructions. Alternatively run the installer shown on the website.

3. Install required Homebrew packages. This installs the closest macOS equivalents to the Debian packages listed above:

```zsh
brew update
brew install git autoconf automake libtool gettext gawk pkg-config \
  libconfuse libunistring ffmpeg libxml2 libgcrypt zlib libevent libplist \
  libsodium json-c curl openssl@1.1 protobuf-c
```

Notes:

- Replace `openssl@1.1` with `openssl` if you want OpenSSL 3. Use `brew info openssl` to check versions.
- macOS uses CoreAudio instead of ALSA (`libasound2-dev`). The project should detect and skip ALSA on macOS; if it doesn't, look for a configure flag to disable ALSA support.

4. Export Homebrew paths so `./configure` finds brewed libraries (portable for Intel/Apple Silicon):

```zsh
export BREW_PREFIX="$(brew --prefix)"
export OPENSSL_PREFIX="$(brew --prefix openssl@1.1)"    # or $(brew --prefix openssl)
export LIBXML2_PREFIX="$(brew --prefix libxml2)"
export ZLIB_PREFIX="$(brew --prefix zlib)"

export PKG_CONFIG_PATH="$OPENSSL_PREFIX/lib/pkgconfig:$LIBXML2_PREFIX/lib/pkgconfig:$ZLIB_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"
export LDFLAGS="-L$OPENSSL_PREFIX/lib -L$LIBXML2_PREFIX/lib -L$ZLIB_PREFIX/lib $LDFLAGS"
export CPPFLAGS="-I$OPENSSL_PREFIX/include -I$LIBXML2_PREFIX/include -I$ZLIB_PREFIX/include $CPPFLAGS"
```

5. Build the project:

```zsh
git submodule update --init
autoreconf -i
./configure
make
```

If `./configure` fails to find libraries, check the `PKG_CONFIG_PATH` and the `--with-...` flags in `./configure --help` and point them to the Homebrew prefixes above.

Troubleshooting:

- If the build fails complaining about ALSA, there should be an option to disable ALSA in the configure script; macOS does not use ALSA.
- Use `brew info <formula>` to find the prefix of any formula. Use those prefixes in `PKG_CONFIG_PATH`, `LDFLAGS` and `CPPFLAGS`.
- On Apple Silicon, Homebrew is usually installed under `/opt/homebrew`. The `$(brew --prefix)` calls above handle that automatically.
