# Local Linux reproduction of the terrain CI jobs (runs-on: ubuntu-26.04), with Mesa's
# software drivers: llvmpipe for OpenGL (EGL surfaceless) and lavapipe for Vulkan.
#
#   docker build -t terrain-linux-ci:26.04 -f scripts/terrain-linux.Dockerfile .github/scripts
#   docker run --rm -v "$PWD":/src -w /src -e EGL_PLATFORM=surfaceless \
#     -e "CMAKE_CONFIGURE_ARGS=-DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_C_COMPILER_LAUNCHER=" \
#     terrain-linux-ci:26.04 bash -c 'git config --global --add safe.directory "*"; scripts/terrain-ci.sh opengl'
#   (same with `vulkan`; builds land in the git-ignored build-linux-opengl / build-linux-vulkan)
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y sudo ca-certificates git clang cmake ninja-build xvfb xauth python3 \
      mesa-utils vulkan-tools libgl1-mesa-dri && rm -rf /var/lib/apt/lists/*
COPY install-linux-deps /tmp/install-linux-deps
RUN bash /tmp/install-linux-deps && rm -rf /var/lib/apt/lists/*
