#!/bin/sh

if test -x /usr/bin/ccache; then
    if test -f /etc/debian_version; then
        CCACHE_PATH=/usr/lib/ccache
    elif test -f /etc/alpine-release; then
        CCACHE_PATH=/usr/lib/ccache/bin
    else
        CCACHE_PATH=/usr/lib64/ccache
    fi

    # Common setup among container builds before we get to building code.

    export CCACHE_COMPILERCHECK=content
    export CCACHE_COMPRESS=true
    export CCACHE_DIR="/cache/$CI_PROJECT_NAME/ccache"
    export PATH="$CCACHE_PATH:$PATH"

    # CMake ignores $PATH, so we have to force CC/GCC to the ccache versions.
    export CC="${CCACHE_PATH}/gcc"
    export CXX="${CCACHE_PATH}/g++"

    ccache --show-stats
fi

# Make a wrapper script for ninja to always include the -j flags
{
    echo '#!/bin/sh -x'
    # shellcheck disable=SC2016
    echo '/usr/bin/ninja -j${FDO_CI_CONCURRENT:-4} "$@"'
} > /usr/local/bin/ninja
chmod +x /usr/local/bin/ninja

# Set MAKEFLAGS so that all make invocations in container builds include the
# flags (doesn't apply to non-container builds, but we don't run make there)
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"

# make wget to try more than once, when download fails or timeout
echo -e "retry_connrefused = on\n" \
        "read_timeout = 300\n" \
        "tries = 4\n" \
	"retry_on_host_error = on\n" \
	"retry_on_http_error = 429,500,502,503,504\n" \
        "wait_retry = 32" >> /etc/wgetrc

# Ensure that rust tools are in PATH if they exist
CARGO_ENV_FILE="$HOME/.cargo/env"
if [ -f "$CARGO_ENV_FILE" ]; then
    # shellcheck disable=SC1090
    source "$CARGO_ENV_FILE"
fi
