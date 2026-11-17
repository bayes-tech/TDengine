#!/bin/bash

# Copyright (c) 2026 TAOS Data, Inc. <jhtao@taosdata.com>
#
# pack_community_tar.sh — Generate community edition tar.xz packages (server + client)
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can use, redistribute, and/or modify
# it under the terms of the GNU Affero General Public License, version 3
# or later ("AGPL"), as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.
#
# Output:
#   <community_dir>/release/TDengine-server-<ver>-Linux-<arch>.tar.xz
#   <community_dir>/release/TDengine-client-<ver>-Linux-<arch>.tar.xz
# For TSDB-OSS release names, use pack_oss_tar.sh.
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

set -e

# ======================== Default Values ========================

version=""
versionComp=""
verType=""
compile_dir=""
release_dir=""
format="legacy"

productName="TDengine"
clientName="taos"
serverName="taosd"
configFile="taos.cfg"

# ======================== Parse Arguments ========================

while getopts "hc:n:m:V:o:f:" arg; do
  case $arg in
    c) compile_dir="$OPTARG" ;;
    n) version="$OPTARG" ;;
    m) versionComp="$OPTARG" ;;
    V) verType="$OPTARG" ;;
    o) release_dir="$OPTARG" ;;
    f) format="$OPTARG" ;;
    h)
      echo "Usage: $(basename "$0") -c <compile_dir> [-n <version>] [-m <compat_version>] [-V stable|beta|preRelease] [-o <output_dir>] [-f legacy|oss]"
      echo ""
      echo "  -c  Compile directory (cmake build output, e.g. debug/)"
      echo "  -n  Version number (e.g. 3.3.6.0)"
      echo "  -m  Compatible version number (default: 3.0.0.0)"
      echo "  -V  Version type: stable | beta | preRelease"
      echo "  -o  Output directory (default: source release/)"
      echo "  -f  Archive naming: legacy or oss (OSS reads version metadata from CMakeCache.txt)"
      exit 0
      ;;
    ?)
      echo "Unknown argument. Use -h for help."
      exit 1
      ;;
  esac
done

# ======================== Validate Inputs ========================

if [ -z "$compile_dir" ]; then
  echo "Error: compile directory (-c) is required"
  exit 1
fi

if [[ "$format" != legacy && "$format" != oss ]]; then
  echo "Error: archive format must be 'legacy' or 'oss'" >&2
  exit 1
fi

script_dir="$(dirname "$(readlink -f "$0")")"
community_dir="$(readlink -f "${script_dir}/..")"
release_dir=${release_dir:-"${community_dir}/release"}

# Preserve source-relative paths for the legacy entry point.
if [[ "$format" == legacy && "$compile_dir" != /* ]]; then
  compile_dir="${community_dir}/${compile_dir}"
fi
compile_dir="$(cd -- "$compile_dir" && pwd)" || exit 1
build_dir="${compile_dir}/build"
code_dir="${community_dir}"
cfg_dir="${community_dir}/packaging/cfg"
tools_dir="${community_dir}/packaging/tools"

if [[ "$format" == oss ]]; then
  cache_file="${compile_dir}/CMakeCache.txt"
  if [[ ! -r "$cache_file" ]]; then
    echo "Error: missing CMake build metadata: $cache_file" >&2
    exit 1
  fi
  cache_value() {
    sed -n "s/^${1}:[^=]*=//p" "$cache_file"
  }
  edition=$(cache_value BUILD_ENTERPRISE)
  case "${edition^^}" in
    OFF|FALSE|0|NO) ;;
    *) echo "Error: OSS packages require a community build (BUILD_ENTERPRISE=OFF)" >&2; exit 1 ;;
  esac
  build_version=$(cache_value BUILD_VER_NUMBER)
  build_compat=$(cache_value BUILD_VER_COMPATIBLE)
  build_type=$(cache_value BUILD_VER_TYPE)
  version=${version:-"$build_version"}
  versionComp=${versionComp:-"$build_compat"}
  verType=${verType:-"$build_type"}
  if [[ -z "$version" || -z "$versionComp" || -z "$verType" ||
        "$version" != "$build_version" || "$versionComp" != "$build_compat" || "$verType" != "$build_type" ]]; then
    echo "Error: package version, compatibility version and release type must match $cache_file" >&2
    exit 1
  fi
else
  if [[ -z "$version" ]]; then
    echo "Error: version number (-n) is required for legacy packages" >&2
    exit 1
  fi
  versionComp=${versionComp:-3.0.0.0}
  verType=${verType:-stable}
fi

version_pattern='^([0-9]+\.){2,4}[0-9]+$'
if [[ ! "$version" =~ $version_pattern ]]; then
  echo "Error: expected a numeric version (e.g. 3.4.3.9)" >&2
  exit 1
fi
if [[ "$format" == oss && ! "$versionComp" =~ $version_pattern ]]; then
  echo "Error: expected a numeric compatibility version (e.g. 3.0.0.0)" >&2
  exit 1
fi
if [[ "$verType" != stable && "$verType" != beta && "$verType" != preRelease ]]; then
  echo "Error: release type must be 'stable', 'beta', or 'preRelease'" >&2
  exit 1
fi

# Copy the release subset from packaging/examples-manifest.txt.
copy_examples() {
  local dst_dir=$1
  local examples_src="${community_dir}/docs/examples"
  local examples_manifest="${community_dir}/packaging/examples-manifest.txt"

  mkdir -p "${dst_dir}/examples"
  while read -r ex_group ex_dst ex_src; do
    case "${ex_group}" in "" | \#*) continue ;; esac
    [ "${ex_group}" = all ] || continue
    if [[ "$format" == oss ]]; then
      ex_dst=${ex_src%%/*}
    else
      [ "${ex_dst}" = c ] || continue
    fi
    if [ ! -f "${examples_src}/${ex_src}" ]; then
      echo "Error: example source missing: ${ex_src} (see ${examples_manifest})" >&2
      exit 1
    fi
    # Keep the path below the language directory; see makepkg.sh.
    ex_rel="${ex_src#*/}"
    mkdir -p "${dst_dir}/examples/${ex_dst}/$(dirname "${ex_rel}")"
    cp "${examples_src}/${ex_src}" "${dst_dir}/examples/${ex_dst}/${ex_rel}"
  done < "${examples_manifest}"
}

# Verify build directory exists
if [ ! -d "$build_dir" ]; then
  echo "Error: build directory not found: $build_dir"
  echo "Please run cmake and make before packaging."
  exit 1
fi

# Detect OS type
os_type=$(uname)
if [ "$os_type" != "Linux" ]; then
  echo "Error: this script only supports Linux. Detected: $os_type"
  exit 1
fi

# Detect CPU architecture
arch_raw=$(uname -m)
case "$arch_raw" in
  x86_64)       arch="x64" ;;
  aarch64)      arch="arm64" ;;
  arm64)        arch="arm64" ;;
  mips64*)      arch="mips64" ;;
  loongarch64)  arch="loongarch64" ;;
  riscv64)      arch="riscv64" ;;
  *)
    echo "Warning: unrecognized architecture '$arch_raw', using as-is"
    arch="$arch_raw"
    ;;
esac

# ======================== Verify Required Files ========================

# Required binaries
for bin in ${serverName} ${clientName}; do
  if [ ! -f "${build_dir}/bin/${bin}" ]; then
    echo "Error: required binary not found: ${build_dir}/bin/${bin}"
    exit 1
  fi
done

# Required libraries
if [ ! -f "${build_dir}/lib/libtaos.so" ]; then
  echo "Error: required library not found: ${build_dir}/lib/libtaos.so"
  exit 1
fi
if [ ! -f "${build_dir}/lib/libtaosnative.so" ]; then
  echo "Error: required library not found: ${build_dir}/lib/libtaosnative.so"
  exit 1
fi

# Required packaging files
for f in \
  "${tools_dir}/install.sh" \
  "${tools_dir}/install_client.sh" \
  "${tools_dir}/remove.sh" \
  "${tools_dir}/remove_client.sh" \
  "${cfg_dir}/${configFile}"; do
  if [ ! -f "$f" ]; then
    echo "Error: required packaging file not found: $f"
    exit 1
  fi
done

if [[ "$format" == oss ]]; then
  elf_machine() {
    LC_ALL=C readelf -h "$1" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p'
  }
  machine=$(elf_machine "${build_dir}/bin/${serverName}")
  case "$machine" in
    'Advanced Micro Devices X86-64') arch=x64 ;;
    AArch64) arch=arm64 ;;
    *) echo "Error: unsupported OSS ELF architecture: $machine" >&2; exit 1 ;;
  esac
  for artifact in "bin/${clientName}" lib/libtaos.so lib/libtaosnative.so; do
    if [[ $(elf_machine "${build_dir}/${artifact}") != "$machine" ]]; then
      echo "Error: artifact architecture differs from taosd: $artifact" >&2
      exit 1
    fi
  done

  # Verify the actual binary versions on the target architecture to catch a
  # build directory that was reconfigured without rebuilding the binaries.
  if [[ "$arch_raw" == x86_64 && "$arch" == x64 || "$arch_raw" == aarch64 && "$arch" == arm64 ]]; then
    for binary in "$serverName" "$clientName"; do
      reported=$(LD_LIBRARY_PATH="${build_dir}/lib" "${build_dir}/bin/${binary}" -V)
      if ! grep -Fxq "${binary} version: ${version}.community compatible_version: ${versionComp}" <<< "$reported"; then
        echo "Error: binary version does not match the package metadata: $binary" >&2
        exit 1
      fi
    done
  fi
  os_type=linux
  server_pkg_name="tdengine-tsdb-oss-${version}"
  client_pkg_name="tdengine-tsdb-oss-client-${version}"
else
  server_pkg_name="${productName}-server-${version}"
  client_pkg_name="${productName}-client-${version}"
fi

release_suffix=""
if [[ "$verType" != stable ]]; then
  release_suffix="-${verType}"
fi
server_tar_name="${server_pkg_name}${release_suffix}-${os_type}-${arch}.tar.xz"
client_tar_name="${client_pkg_name}${release_suffix}-${os_type}-${arch}.tar.xz"

mkdir -p -- "$release_dir"
release_dir="$(cd -- "$release_dir" && pwd)"
stage_dir="$(mktemp -d "${release_dir}/.tdengine-package.XXXXXX")"
trap 'rm -rf -- "$stage_dir"' EXIT

printf 'Packaging %s (%s, %s %s) into %s\n' "$version" "$verType" "$os_type" "$arch" "$release_dir"

# ======================== Helper Functions ========================

# Copy a file if it exists; skip silently otherwise
copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [ -f "$src" ]; then
    cp "$src" "$dst"
  fi
}

patch_server_install_messages() {
  local installer="$1"

  # The shared install.sh prints guidance for keeper/explorer/UI unconditionally.
  # Community tar packages do not ship those components, so trim the misleading
  # guidance from the copied installer.
  sed -i \
    -e '/To configure \${keeperName}:/d' \
    -e '/To configure \${xname}:/d' \
    -e '/To configure \${explorerName}:/d' \
    -e '/To start \${keeperName}:/d' \
    -e '/To start \${explorerName}:/d' \
    -e '/To access \${productName} GUI:/d' \
    "$installer"
}

# ======================== Build Server Package ========================

echo ""
echo ">>> Building server package..."

server_dir="${stage_dir}/${server_pkg_name}"
mkdir -p "${server_dir}"

# --- Inner package: bin/ ---
mkdir -p "${server_dir}/bin"

# Core binaries (required)
cp "${build_dir}/bin/${serverName}" "${server_dir}/bin/"
cp "${build_dir}/bin/${clientName}" "${server_dir}/bin/"

# Optional binaries (may or may not be built)
copy_if_exists "${build_dir}/bin/taosBenchmark" "${server_dir}/bin/"
copy_if_exists "${build_dir}/bin/taosdump"      "${server_dir}/bin/"
copy_if_exists "${build_dir}/bin/taosudf"       "${server_dir}/bin/"

# Packaging helper scripts
copy_if_exists "${tools_dir}/remove.sh"         "${server_dir}/bin/"
copy_if_exists "${tools_dir}/set_core.sh"       "${server_dir}/bin/"
copy_if_exists "${tools_dir}/startPre.sh"       "${server_dir}/bin/"
copy_if_exists "${tools_dir}/taosd-dump-cfg.gdb" "${server_dir}/bin/"

chmod a+x "${server_dir}/bin/"* 2>/dev/null || :

# --- Inner package: cfg/ ---
mkdir -p "${server_dir}/cfg"
copy_if_exists "${cfg_dir}/${configFile}"     "${server_dir}/cfg/"
copy_if_exists "${cfg_dir}/${serverName}.service" "${server_dir}/cfg/"
install -Dm644 "${community_dir}/packaging/deb/taosd" "${server_dir}/init.d/taosd.deb"
install -Dm644 "${community_dir}/packaging/rpm/taosd" "${server_dir}/init.d/taosd.rpm"
if [[ "$format" == oss ]]; then
  copy_if_exists "${tools_dir}/set_taos_malloc.sh" "${server_dir}/bin/"
  if [[ ! -x "${server_dir}/bin/taosudf" ]]; then
    sed -i '/^[[:space:]]*udf[[:space:]]/d' "${server_dir}/cfg/${configFile}"
    printf '\nudf 0\n' >> "${server_dir}/cfg/${configFile}"
  fi
fi

# --- Inner package: inc/ ---
mkdir -p "${server_dir}/inc"
for header in \
  "${code_dir}/include/client/taos.h" \
  "${code_dir}/include/common/taosdef.h" \
  "${code_dir}/include/util/taoserror.h" \
  "${code_dir}/include/util/tdef.h" \
  "${code_dir}/include/libs/function/taosudf.h"; do
  copy_if_exists "$header" "${server_dir}/inc/"
done

# --- Outer package: driver/ ---
mkdir -p "${server_dir}/driver"
cp "${build_dir}/lib/libtaos.so" "${server_dir}/driver/libtaos.so.${version}"
cp "${build_dir}/lib/libtaosnative.so" "${server_dir}/driver/libtaosnative.so.${version}"
echo "${versionComp}" > "${server_dir}/driver/vercomp.txt"

# --- Outer package: examples/ ---
copy_examples "${server_dir}"

# --- Outer package: install.sh ---
cp "${tools_dir}/install.sh" "${server_dir}/"
sed -i 's/verMode=cluster/verMode=edge/g' "${server_dir}/install.sh" 2>/dev/null || :
patch_server_install_messages "${server_dir}/install.sh"
chmod a+x "${server_dir}/install.sh"

# The community packages have one managed service, taosd.
for action in start stop; do
  {
    printf '#!/bin/bash\naction=%s\n' "$action"
    cat <<'SERVICEEOF'
if [[ $(id -u) -ne 0 ]]; then
    exec systemctl --user "$action" taosd
fi
if [[ ! -d /run/systemd/system && -x /etc/init.d/taosd ]]; then
    exec service taosd "$action"
fi
exec systemctl "$action" taosd
SERVICEEOF
  } > "${server_dir}/${action}-all.sh"
  chmod a+x "${server_dir}/${action}-all.sh"
done

# ======================== Build Client Package ========================

echo ""
echo ">>> Building client package..."

client_dir="${stage_dir}/${client_pkg_name}"
mkdir -p "${client_dir}"

# --- Inner package: bin/ ---
mkdir -p "${client_dir}/bin"

# Core binary
cp "${build_dir}/bin/${clientName}" "${client_dir}/bin/"

# Optional tools
copy_if_exists "${build_dir}/bin/taosBenchmark" "${client_dir}/bin/"
copy_if_exists "${build_dir}/bin/taosdump"      "${client_dir}/bin/"

# Packaging helper scripts
copy_if_exists "${tools_dir}/remove_client.sh"  "${client_dir}/bin/"
copy_if_exists "${tools_dir}/set_core.sh"       "${client_dir}/bin/"
copy_if_exists "${tools_dir}/get_client.sh"     "${client_dir}/bin/"

chmod a+x "${client_dir}/bin/"* 2>/dev/null || :

# --- Inner package: cfg/ ---
mkdir -p "${client_dir}/cfg"
copy_if_exists "${cfg_dir}/${configFile}" "${client_dir}/cfg/"

# --- Inner package: inc/ ---
mkdir -p "${client_dir}/inc"
for header in \
  "${code_dir}/include/client/taos.h" \
  "${code_dir}/include/common/taosdef.h" \
  "${code_dir}/include/util/taoserror.h" \
  "${code_dir}/include/util/tdef.h" \
  "${code_dir}/include/libs/function/taosudf.h"; do
  copy_if_exists "$header" "${client_dir}/inc/"
done

# --- Outer package: driver/ ---
mkdir -p "${client_dir}/driver"
cp "${build_dir}/lib/libtaos.so" "${client_dir}/driver/libtaos.so.${version}"
cp "${build_dir}/lib/libtaosnative.so" "${client_dir}/driver/libtaosnative.so.${version}"
echo "${versionComp}" > "${client_dir}/driver/vercomp.txt"

# --- Outer package: examples/ ---
copy_examples "${client_dir}"

# --- Outer package: install_client.sh ---
cp "${tools_dir}/install_client.sh" "${client_dir}/"
sed -i 's/verMode=cluster/verMode=edge/g' "${client_dir}/install_client.sh" 2>/dev/null || :
chmod a+x "${client_dir}/install_client.sh"

if [[ "$format" == oss ]]; then
  cp "${community_dir}/LICENSE" "${server_dir}/"
  cp "${community_dir}/LICENSE" "${client_dir}/"
  printf '# TDengine TSDB-OSS %s\n\nRun `sudo bash ./install.sh -s -e no` to install the native server and client.\n' \
    "$version" > "${server_dir}/README.md"
  printf '# TDengine TSDB-OSS Client %s\n\nRun `sudo bash ./install_client.sh` to install the native client.\n' \
    "$version" > "${client_dir}/README.md"
fi

echo "Creating ${server_tar_name} and ${client_tar_name}..."
tar -C "$stage_dir" -Jcf "${stage_dir}/${server_tar_name}" "$server_pkg_name"
tar -C "$stage_dir" -Jcf "${stage_dir}/${client_tar_name}" "$client_pkg_name"
if [[ "$format" == oss ]]; then
  checksum_name="SHA256SUMS-${version}${release_suffix}-${os_type}-${arch}.txt"
  (
    cd "$stage_dir"
    sha256sum "$server_tar_name" "$client_tar_name" > "$checksum_name"
  )
fi
mv -- "${stage_dir}/${server_tar_name}" "${stage_dir}/${client_tar_name}" "$release_dir/"
if [[ "$format" == oss ]]; then
  mv -- "${stage_dir}/${checksum_name}" "$release_dir/"
fi
ls -lh "${release_dir}/${server_tar_name}" "${release_dir}/${client_tar_name}"
