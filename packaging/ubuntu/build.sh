#!/usr/bin/env bash
set -euo pipefail

src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
arch=$1
shift
case $arch in
  arm64) machine=aarch64; deb_arch=arm64; elf_machine=AArch64 ;;
  x64) machine=x86_64; deb_arch=amd64; elf_machine='Advanced Micro Devices X86-64' ;;
  *) printf '%s\n' 'Supported Ubuntu targets are arm64 and x64.' >&2; exit 1 ;;
esac
build=${1:-"$PWD/tdengine-build-ubuntu-$arch"}
version=${TD_VERSION:-3.4.3.9}
jobs=${TD_JOBS:-4}
rocksdb_from_source=${TD_BUILD_ROCKSDB:-OFF}

if [[ $build == -h || $build == --help ]]; then
  printf '%s\n' 'Usage: build.sh [build-directory]' \
    'Environment: TD_VERSION=3.4.3.9 TD_JOBS=4 TD_BUILD_ROCKSDB=OFF'
  exit 0
fi

if [[ $(uname -s) != Linux || $(uname -m) != "$machine" || $(dpkg --print-architecture) != "$deb_arch" ]]; then
  printf 'Run this script in an Ubuntu %s environment, or use build-image.sh.\n' "$arch" >&2
  exit 1
fi
if [[ ! $jobs =~ ^[1-9][0-9]*$ || ! $version =~ ^([0-9]+\.){2,4}[0-9]+$ ]]; then
  printf '%s\n' 'Set TD_JOBS to a positive integer and TD_VERSION to a numeric version.' >&2
  exit 1
fi

case $rocksdb_from_source in
  ON) rocksdb_use_deps=OFF ;;
  OFF) rocksdb_use_deps=ON ;;
  *) printf '%s\n' 'TD_BUILD_ROCKSDB must be ON or OFF.' >&2; exit 1 ;;
esac

mkdir -p -- "$build"
build=$(cd -- "$build" && pwd)
if [[ $build == "$src" ]]; then
  printf '%s\n' 'Choose a separate build directory.' >&2
  exit 1
fi
if ! printf '#include <termio.h>\n' | "${CC:-cc}" -E -x c - >/dev/null 2>&1; then
  mkdir -p "$build/compat-include"
  printf '#include <termios.h>\n#include <sys/ioctl.h>\n' > "$build/compat-include/termio.h"
  export C_INCLUDE_PATH="$build/compat-include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
fi

cmake -S "$src" -B "$build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON \
  -DBUILD_ENTERPRISE=OFF \
  -DBUILD_VER_CPUTYPE:STRING="$arch" \
  -DBUILD_VER_NUMBER="$version" \
  -DBUILD_VER_COMPATIBLE=3.0.0.0 \
  -DBUILD_VER_TYPE=preRelease \
  -DBUILD_CONTRIB=ON \
  -DBUILD_USE_PUBLIC_DEPS=ON \
  -DTD_EXTERNALS_BASE_DIR="$build/.externals" \
  -DTD_EXTERNALS_BUILD_JOBS="$jobs" \
  -DBUILD_ROCKSDB="$rocksdb_from_source" \
  -DROCKSDB_USE_DEPS="$rocksdb_use_deps" \
  -DBUILD_TEST=OFF \
  -DBUILD_DEPENDENCY_TESTS=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_WEBSOCKET=OFF \
  -DRUST_BINDINGS=OFF \
  -DBUILD_WITH_UDF=OFF \
  -DBUILD_PYUDF=OFF \
  -DBUILD_WITH_ANALYSIS=OFF
cmake --build "$build" --target taosd taos shell --parallel "$jobs"

export LD_LIBRARY_PATH="$build/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
for artifact in bin/taosd bin/taos lib/libtaos.so lib/libtaosnative.so; do
  path="$build/build/$artifact"
  LC_ALL=C readelf -h "$path" | grep -E "Machine:[[:space:]]+${elf_machine}$"
  dependencies=$(ldd "$path")
  if [[ $dependencies == *'not found'* ]]; then
    printf '%s\n' "$path" "$dependencies" >&2
    exit 1
  fi
done
"$build/build/bin/taosd" -V
"$build/build/bin/taos" -V
printf 'Artifacts: %s/build\n' "$build"
