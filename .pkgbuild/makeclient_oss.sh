#!/bin/bash
#
# Build an official-layout TDengine OSS client (driver) gz package from a local
# build tree. Mirrors packaging/tools/makeclient.sh, minus the enterprise-only
# inputs / network downloads that do not apply to a standalone OSS checkout.
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
clientName="taos"
configFile="taos.cfg"

install_dir="${release_dir}/${productName}-client-${version}"
pkg_name="${install_dir}-${osType}-${cpuType}"

rm -rf "${install_dir}"
mkdir -p "${install_dir}"/{inc,cfg,bin}

# ---- headers -------------------------------------------------------------
for f in taos.h taosdef.h taoserror.h tdef.h taosudf.h; do
  case "${f}" in
    taos.h)         cp "${top_dir}/include/client/taos.h" "${install_dir}/inc/" ;;
    taosdef.h)      cp "${top_dir}/include/common/taosdef.h" "${install_dir}/inc/" ;;
    taoserror.h)    cp "${top_dir}/include/util/taoserror.h" "${install_dir}/inc/" ;;
    tdef.h)         cp "${top_dir}/include/util/tdef.h" "${install_dir}/inc/" ;;
    taosudf.h)      cp "${top_dir}/include/libs/function/taosudf.h" "${install_dir}/inc/" ;;
  esac
done
[ -f "${build_dir}/include/taosws.h" ] && cp "${build_dir}/include/taosws.h" "${install_dir}/inc/" || true

# ---- config --------------------------------------------------------------
cp "${cfg_dir}/${configFile}" "${install_dir}/cfg/${configFile}"

# ---- binaries ------------------------------------------------------------
for b in taos taosBenchmark taosdump; do
  if [ -f "${build_dir}/bin/${b}" ]; then
    cp "${build_dir}/bin/${b}" "${install_dir}/bin/"
  else
    echo "WARN: missing binary ${b}"
  fi
done
for s in remove_client.sh set_core.sh get_client.sh; do
  [ -f "${script_dir}/${s}" ] && cp "${script_dir}/${s}" "${install_dir}/bin/" || true
done
chmod a+x "${install_dir}"/bin/*

# ---- inner payload (bin/cfg/inc), matches makeclient.sh -------------------
cd "${install_dir}"
tar -zcf package.tar.gz bin cfg inc --remove-files
cd "${top_dir}"

# ---- inner install_client.sh ---------------------------------------------
cp "${script_dir}/install_client.sh" "${install_dir}/install_client.sh"
chmod a+x "${install_dir}/install_client.sh"

# ---- driver libs ---------------------------------------------------------
mkdir -p "${install_dir}/driver"
cp "${build_dir}/lib/libtaos.so"       "${install_dir}/driver/libtaos.so.${version}"
cp "${build_dir}/lib/libtaosnative.so" "${install_dir}/driver/libtaosnative.so.${version}"
[ -f "${build_dir}/lib/libtaosws.so" ] && cp "${build_dir}/lib/libtaosws.so" "${install_dir}/driver/libtaosws.so.${version}" || true
printf '%s' "${versionComp}" > "${install_dir}/driver/vercomp.txt"

# ---- examples ------------------------------------------------------------
mkdir -p "${install_dir}/examples"
examples_dir="${top_dir}/examples"
new_example_dir="${top_dir}/docs/examples"
cp -r "${examples_dir}/c" "${install_dir}/examples" || true
for e in matlab python R go nodejs C#; do
  [ -d "${examples_dir}/${e}" ] && cp -r "${examples_dir}/${e}" "${install_dir}/examples" || true
done
[ -d "${new_example_dir}/JDBC" ] && cp -r "${new_example_dir}/JDBC" "${install_dir}/examples" || true
mkdir -p "${install_dir}/examples/taosbenchmark-json"
[ -d "${top_dir}/tools/taos-tools/example" ] && cp "${top_dir}/tools/taos-tools/example/"* "${install_dir}/examples/taosbenchmark-json/" 2>/dev/null || true

# ---- connector (only what exists locally; no network fetch) --------------
mkdir -p "${install_dir}/connector"
for c in go python nodejs; do
  if [ -d "${top_dir}/connector/${c}" ]; then
    cp -r "${top_dir}/connector/${c}" "${install_dir}/connector" || true
  fi
done

# ---- outer package -------------------------------------------------------
cd "${release_dir}"
tar -zcf "$(basename "${pkg_name}").tar.gz" "$(basename "${install_dir}")"
cd "${top_dir}"

echo "package: ${release_dir}/$(basename "${pkg_name}").tar.gz"
ls -lh "${release_dir}/$(basename "${pkg_name}").tar.gz"
