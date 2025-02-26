#!/usr/bin/env bash
# shellcheck disable=SC1091

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# ALPINE_X86_64_BUILD_TAG

set -e

. .gitlab-ci/setup-test-env.sh

set -o xtrace

EPHEMERAL=(
)


DEPS=(
    bash
    bison
    ccache
    "clang${LLVM_VERSION}-dev"
    clang-dev
    cmake
    coreutils
    curl
    elfutils-dev
    expat-dev
    flex
    g++
    gcc
    gettext
    git
    glslang
    graphviz
    libclc-dev
    libdrm-dev
    libpciaccess-dev
    libva-dev
    linux-headers
    "llvm${LLVM_VERSION}-dev"
    "llvm${LLVM_VERSION}-static"
    mold
    musl-dev
    py3-clang
    py3-cparser
    py3-mako
    py3-packaging
    py3-pip
    py3-ply
    py3-yaml
    python3-dev
    samurai
    spirv-llvm-translator-dev
    spirv-tools-dev
    util-macros
    vulkan-headers
    zlib-dev
)

apk --no-cache add "${DEPS[@]}" "${EPHEMERAL[@]}"

pip3 install --break-system-packages sphinx===5.1.1 hawkmoth===0.16.0

. .gitlab-ci/container/container_pre_build.sh

. .gitlab-ci/container/install-meson.sh

EXTRA_MESON_ARGS='--prefix=/usr' \
. .gitlab-ci/container/build-wayland.sh

############### Uninstall the build software

# too many vendor binarise, just keep the ones we need
find /usr/share/clc \
  \( -type f -o -type l \) \
  ! -name 'spirv-mesa3d-.spv' \
  ! -name 'spirv64-mesa3d-.spv' \
  -delete

apk del "${EPHEMERAL[@]}"

. .gitlab-ci/container/container_post_build.sh
