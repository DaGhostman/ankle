#!/bin/sh
#
# PGO build + install of ankle.
#
# Usage: ./build-pgo.sh [--no-install]
#
# Environment:
#   BUILD_DIR  build directory (default: build-pgo)
#   PREFIX     install prefix  (default: ~/.local)
#

set -eu

cd "$(dirname "$(realpath "${0}")")"

blddir=${BUILD_DIR-build-pgo}
prefix=${PREFIX-${HOME}/.local}
install=yes
[ "${1-}" = "--no-install" ] && install=no

export CFLAGS="${CFLAGS-} -O3"
export CCACHE_DISABLE=1
export LC_CTYPE=en_US.UTF-8

# Full PGO needs a Wayland session; fall back to the parser-only helper
if [ -n "${WAYLAND_DISPLAY-}" ]; then
    mode=full-current-session
else
    mode=partial
fi

# meson never re-checks out an existing subproject, so a fcft checkout
# left over from before the wrap's revision was bumped is silently
# reused (without the newer patches). Drop it if it is stale.
fcft_rev=$(sed -n 's/^revision *= *//p' subprojects/fcft.wrap)
if [ -d subprojects/fcft ] &&
   [ "$(git -C subprojects/fcft rev-parse HEAD 2>/dev/null)" != "${fcft_rev}" ]; then
    echo "subprojects/fcft is not at ${fcft_rev}; re-downloading"
    rm -rf subprojects/fcft
fi

set -x

# 1. configure (from scratch)
rm -rf "${blddir}"
meson setup --buildtype=release -Db_lto=true --prefix="${prefix}" "${blddir}" .

# 2. instrumented build
meson configure "${blddir}" -Db_pgo=generate
ninja -C "${blddir}"
ninja -C "${blddir}" test

# 3. generate profiling data
./pgo/${mode}.sh . "${blddir}"

# 4. rebuild using the profile
meson configure "${blddir}" -Db_pgo=use
ninja -C "${blddir}"
ninja -C "${blddir}" test

# 5. install
if [ ${install} = yes ]; then
    if [ -w "${prefix}" ]; then
        ninja -C "${blddir}" install
    else
        sudo ninja -C "${blddir}" install
    fi
fi
