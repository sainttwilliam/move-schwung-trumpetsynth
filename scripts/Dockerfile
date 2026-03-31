# Trumpet Synth — cross-compilation environment
# Targets: aarch64-linux-gnu (Ableton Move, Cortex-A72)
#
# Builds aubio 0.4.9 as a static PIC library with no external FFT or I/O
# dependencies (uses the built-in ooura FFT; enough for YIN pitch detection).
# The static library is baked into the image so the module compile step has
# no network requirements.

FROM debian:bookworm

# Cross-compilation toolchain + aubio build prerequisites
RUN apt-get update && apt-get install -y \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        make \
        file \
        python3 \
        python-is-python3 \
        curl \
        bzip2 \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Cross-compile aubio 0.4.9 for aarch64
#
# Flags:
#   -fPIC          required so the static .a can be linked into a .so
#   -march/-mtune  match the Ableton Move processor (Cortex-A72 / armv8-a)
#   --disable-*    strip all I/O and FFT library dependencies; built-in
#                  ooura FFT is sufficient for YIN pitch detection
# ---------------------------------------------------------------------------
RUN curl -fsSL https://aubio.org/pub/aubio-0.4.9.tar.bz2 | tar -xj -C /tmp \
    && find /tmp/aubio-0.4.9/waflib -name '*.py' -exec sed -i "s/'rU'/'r'/g" {} \; \
    && cd /tmp/aubio-0.4.9 \
    && CC=aarch64-linux-gnu-gcc \
       AR=aarch64-linux-gnu-ar \
       RANLIB=aarch64-linux-gnu-ranlib \
       CFLAGS="-fPIC -O3 -march=armv8-a -mtune=cortex-a72" \
       python3 waf configure \
           --build-type=release \
           --disable-fftw3 \
           --disable-jack \
           --disable-sndfile \
           --disable-avcodec \
           --disable-wavread \
           --disable-wavwrite \
    && python3 waf build || test -f build/src/libaubio.a \
    && install -m 644 build/src/libaubio.a /usr/local/lib/libaubio-aarch64.a \
    && mkdir -p /usr/local/include/aubio \
    && cp -r src/* /usr/local/include/aubio/ \
    && rm -rf /tmp/aubio-0.4.9

WORKDIR /build

ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
ENV CXX=aarch64-linux-gnu-g++
ENV AUBIO_STATIC=/usr/local/lib/libaubio-aarch64.a
ENV AUBIO_INCLUDE=/usr/local/include
