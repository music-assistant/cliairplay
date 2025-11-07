# cliairplay

Command line interface for audio streaming to AirPlay 2 devices

Based on [owntones](https://github.com/owntone/owntone-server) (all rights reserved).

## Pre-built binaries

Pre-built binaries for Linux (x86_64 and ARM64) and macOS (Apple Silicon/ARM64) are automatically built via GitHub Actions and available as build artifacts on the repository.

## Debian build

Install required tools and libraries:

```bash
sudo apt-get install \
  build-essential git autotools-dev autoconf automake libtool pkgconf gettext gawk gperf flex bison \
  uuid-dev zlib1g-dev libcurl4-openssl-dev libsodium-dev \
  libconfuse-dev libunistring-dev libxml2-dev libevent-dev \
  libjson-c-dev libplist-dev libgcrypt20-dev libgpg-error-dev \
  libavfilter-dev

```

Then run the following:

```bash
git clone https://github.com/music-assistant/cliairplay.git
cd cliairplay
git submodule update --init
autoreconf -fi
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
brew install git autoconf automake libtool pkgconf gettext gawk \
  confuse libunistring ffmpeg libxml2 libgcrypt zlib libevent libplist \
  libiconv libsodium json-c curl openssl@3 protobuf-c bison
```

Notes:

- OpenSSL 3 is recommended as OpenSSL 1.1 reached end-of-life in September 2023.
- Bison from Homebrew is required as macOS ships with an outdated version (2.3).
- macOS uses CoreAudio instead of ALSA (`libasound2-dev`). The project should detect and skip ALSA on macOS; if it doesn't, look for a configure flag to disable ALSA support.

4. Export Homebrew paths so `./configure` finds libraries (portable for Intel/Apple Silicon):

```zsh
export BREW_PREFIX="$(brew --prefix)"
export OPENSSL_PREFIX="$(brew --prefix openssl@3)"
export LIBXML2_PREFIX="$(brew --prefix libxml2)"
export ZLIB_PREFIX="$(brew --prefix zlib)"
export LIBGCRYPT_PREFIX="$(brew --prefix libgcrypt)"
export LIBGPG_ERROR_PREFIX="$(brew --prefix libgpg-error)"
export LIBUNISTRING_PREFIX="$(brew --prefix libunistring)"
export LIBICONV_PREFIX="$(brew --prefix libiconv)"

export PKG_CONFIG_PATH="$LIBXML2_PREFIX/lib/pkgconfig:$ZLIB_PREFIX/lib/pkgconfig:$LIBGCRYPT_PREFIX/lib/pkgconfig:$LIBGPG_ERROR_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"
export LDFLAGS="-L$LIBXML2_PREFIX/lib -L$ZLIB_PREFIX/lib -L$LIBGCRYPT_PREFIX/lib -L$LIBGPG_ERROR_PREFIX/lib -L$LIBICONV_PREFIX/lib"
export CPPFLAGS="-I$OPENSSL_PREFIX/include -I$LIBXML2_PREFIX/include -I$ZLIB_PREFIX/include -I$LIBGCRYPT_PREFIX/include -I$LIBGPG_ERROR_PREFIX/include -I$LIBICONV_PREFIX/include"
export LIBUNISTRING_CFLAGS="-I$LIBUNISTRING_PREFIX/include"
export LIBUNISTRING_LIBS="-L$LIBUNISTRING_PREFIX/lib -lunistring -L$LIBICONV_PREFIX/lib -liconv"
export PATH="$BREW_PREFIX/opt/bison/bin:$PATH"

# For static linking of OpenSSL (recommended for distribution)
export LIBS="$OPENSSL_PREFIX/lib/libssl.a $OPENSSL_PREFIX/lib/libcrypto.a"

```
5. Clone the repo and apply ffmpeg8 patch

```zsh
git clone https://github.com/music-assistant/cliairplay.git
cd cliairplay
git submodule update --init

# Apply FFmpeg 8.0 compatibility patch to owntone-server/src/transcode.c 
cat > /tmp/ffmpeg8.patch << 'EOF'
--- a/owntone-server/src/transcode.c
+++ b/owntone-server/src/transcode.c
@@ -1441,8 +1441,10 @@ transcode_decode_setup_raw(enum transcode_profile profile, struct media_quality
    // If the source has REPLAYGAIN_TRACK_GAIN metadata, this will inject the
    // values into the the next packet's side data (as AV_FRAME_DATA_REPLAYGAIN),
    // which has the effect that a volume replaygain filter works. Note that
    // ffmpeg itself uses another method in process_input() in ffmpeg.c.
+#if LIBAVFORMAT_VERSION_MAJOR < 60
    av_format_inject_global_side_data(ctx->ifmt_ctx);
+#endif

    ret = avformat_find_stream_info(ctx->ifmt_ctx, NULL);
    if (ret < 0)
EOF
patch -p1 < /tmp/ffmpeg8.patch

```

5. Build the project:

```zsh
autoreconf -fi
./configure
make
```

The binary will be created at `src/cliap2`.

If `./configure` fails to find libraries, check the `PKG_CONFIG_PATH` and the `--with-...` flags in `./configure --help` and point them to the Homebrew prefixes above.

Troubleshooting:

- If the build fails complaining about ALSA, there should be an option to disable ALSA in the configure script; macOS does not use ALSA.
- Use `brew info <formula>` to find the prefix of any formula. Use those prefixes in `PKG_CONFIG_PATH`, `LDFLAGS` and `CPPFLAGS`.
- On Apple Silicon, Homebrew is usually installed under `/opt/homebrew`. The `$(brew --prefix)` calls above handle that automatically.
