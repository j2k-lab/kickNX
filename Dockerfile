FROM devkitpro/devkita64:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    pkg-config \
    git \
    python3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN dkp-pacman -Syu --needed --noconfirm \
    switch-dev \
    switch-glfw \
    switch-mesa \
    switch-curl \
    switch-libwebp \
    switch-libmpv \
    switch-ffmpeg \
    switch-mbedtls \
    switch-zlib \
    switch-bzip2 \
    switch-freetype \
    switch-harfbuzz \
    switch-libfribidi \
    switch-libass \
    switch-libpng \
    switch-dav1d

WORKDIR /workspace
