#!/bin/bash
#
# Build an official-layout TDengine OSS gz package from a local build tree.
# Mirrors packaging/tools/makepkg.sh, minus the enterprise-only inputs that do
# not exist in a standalone open-source checkout.
#
set -e

top_dir="/home/luhd/BAYES/TDengine"
compile_dir="${top_dir}/release"
release_dir="${top_dir}/release"
build_dir="${compile_dir}/build"
script_dir="${top_dir}/packaging/tools"
cfg_dir="${top_dir}/packaging/cfg"

version="${1:-3.3.6.48}"
build_time="${2:-$(date +%Y%m%d%H%M)}"
cpuType="${3:-x64}"
osType="${4:-Linux}"
versionComp="${5:-3.0.0.0}"

productName="TDengine"
serverName="taosd"
clientName="taos"
configFile="taos.cfg"

install_dir="${release_dir}/${productName}-server-${version}"
pkg_name="${install_dir}-${osType}-${cpuType}"

rm -rf "${install_dir}"
mkdir -p "${install_dir}"/{inc,cfg,bin,init.d}

# ---- headers -------------------------------------------------------------
header_files="${top_dir}/include/client/taos.h \
  ${top_dir}/include/common/taosdef.h \
  ${top_dir}/include/util/taoserror.h \
  ${top_dir}/include/util/tdef.h \
  ${top_dir}/include/libs/function/taosudf.h"
for f in ${header_files}; do cp "${f}" "${install_dir}/inc/"; done
[ -f "${build_dir}/include/taosws.h" ] && cp "${build_dir}/include/taosws.h" "${install_dir}/inc/" || true

# ---- config --------------------------------------------------------------
cp "${cfg_dir}/${configFile}" "${install_dir}/cfg/${configFile}"
[ -f "${compile_dir}/test/cfg/taosadapter.toml" ]    && cp "${compile_dir}/test/cfg/taosadapter.toml"    "${install_dir}/cfg/" || true
[ -f "${compile_dir}/test/cfg/taoskeeper.toml" ]     && cp "${compile_dir}/test/cfg/taoskeeper.toml"     "${install_dir}/cfg/" || true
[ -f "${compile_dir}/test/cfg/taosadapter.service" ] && cp "${compile_dir}/test/cfg/taosadapter.service" "${install_dir}/cfg/" || true
[ -f "${compile_dir}/test/cfg/taoskeeper.service" ]  && cp "${compile_dir}/test/cfg/taoskeeper.service"  "${install_dir}/cfg/" || true
[ -f "${cfg_dir}/${serverName}.service" ] && cp "${cfg_dir}/${serverName}.service" "${install_dir}/cfg/" || true

# ---- binaries ------------------------------------------------------------
for b in taosd taos taosadapter taoskeeper taosudf taosdump taosBenchmark; do
  if [ -f "${build_dir}/bin/${b}" ]; then
    cp "${build_dir}/bin/${b}" "${install_dir}/bin/"
  else
    echo "WARN: missing binary ${b}"
  fi
done
for s in remove.sh set_core.sh startPre.sh quick_deploy.sh taosd-dump-cfg.gdb; do
  [ -f "${script_dir}/${s}" ] && cp "${script_dir}/${s}" "${install_dir}/bin/" || true
done
chmod a+x "${install_dir}"/bin/*

# ---- init.d --------------------------------------------------------------
cp "${top_dir}/packaging/deb/taosd" "${install_dir}/init.d/${serverName}.deb"
cp "${top_dir}/packaging/rpm/taosd" "${install_dir}/init.d/${serverName}.rpm"

# ---- inner payload (bin/cfg/inc/init.d), matches makepkg.sh ---------------
cd "${install_dir}"
tar -zcf package.tar.gz bin cfg inc init.d --remove-files
cd "${top_dir}"

# ---- inner install.sh ----------------------------------------------------
cp "${script_dir}/install.sh" "${install_dir}/install.sh"
chmod a+x "${install_dir}/install.sh"

# start-all.sh / stop-all.sh ship only from the sibling enterprise repo; install.sh
# copies them unguarded and runs under `set -e`, so an OSS-only package aborts there.
sed -i \
  -e 's|cp "${script_dir}/start-all.sh" "${install_main_dir}/bin"|[ -f "${script_dir}/start-all.sh" ] \&\& cp "${script_dir}/start-all.sh" "${install_main_dir}/bin" \|\| :|' \
  -e 's|cp "${script_dir}/stop-all.sh" "${install_main_dir}/bin"|[ -f "${script_dir}/stop-all.sh" ] \&\& cp "${script_dir}/stop-all.sh" "${install_main_dir}/bin" \|\| :|' \
  "${install_dir}/install.sh"

# ---- driver libs (top level, next to install.sh) -------------------------
mkdir -p "${install_dir}/driver"
cp "${build_dir}/lib/libtaos.so"       "${install_dir}/driver/libtaos.so.${version}"
cp "${build_dir}/lib/libtaosnative.so" "${install_dir}/driver/libtaosnative.so.${version}"
[ -f "${build_dir}/lib/libtaosws.so" ] && cp "${build_dir}/lib/libtaosws.so" "${install_dir}/driver/libtaosws.so.${version}" || true
printf '%s' "${versionComp}" > "${install_dir}/driver/vercomp.txt"

# ---- outer package -------------------------------------------------------
cd "${release_dir}"
tar -zcf "$(basename "${pkg_name}").tar.gz" "$(basename "${install_dir}")"
cd "${top_dir}"

echo "package: ${release_dir}/$(basename "${pkg_name}").tar.gz"
ls -lh "${release_dir}/$(basename "${pkg_name}").tar.gz"
