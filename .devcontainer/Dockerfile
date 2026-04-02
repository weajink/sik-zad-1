FROM mcr.microsoft.com/devcontainers/cpp:ubuntu

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang \
    clang-format \
    clang-tidy \
    valgrind \
    gdb \
    cmake \
    ninja-build \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*
