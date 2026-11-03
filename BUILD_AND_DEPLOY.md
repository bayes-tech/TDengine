# 数据重整版本：编译、打包与部署

本文针对本仓库的数据重整改动，给出 Linux 原生连接模式下的服务端、C 客户端库、命令行客户端和社区版 `.tar.xz` 部署包的操作步骤。功能说明见 [COMPACTION.md](COMPACTION.md)。

服务端和发起 COMPACT 的客户端都应使用本次修改后的代码：旧开源客户端可能在 SQL 翻译阶段直接拒绝命令。以下部署命令供目标机器执行；本次文档编写没有修改本机已安装的 TDengine 服务。

## 1. 构建环境

使用与目标部署机器相同的 CPU 架构，并在不高于目标机器 glibc / libstdc++ 基线的环境中构建。不要直接把 Fedora 新版本上生成的二进制当作可在旧 Ubuntu / CentOS 上运行的通用包。首次配置和编译会下载第三方源码；需要能访问 CMake 输出中列出的公共依赖地址。

仓库顶层要求 CMake >= 3.16；第三方依赖也有自己的工具链要求。准备 GCC/G++、GNU Make、Git、Perl、Python 3、curl、tar、xz、pkg-config 和 autotools。下面是常见发行版的依赖安装示例，并非已经在每个发行版上验证过的最小依赖集合。

Ubuntu / Debian：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git curl ca-certificates \
  tar xz-utils pkg-config perl libtext-template-perl \
  autoconf automake libtool python3 python3-venv python3-pip \
  zlib1g-dev libssl-dev libcurl4-openssl-dev libxml2-dev
```

Fedora：

```bash
sudo dnf install -y gcc gcc-c++ make cmake git curl ca-certificates \
  tar xz pkgconf-pkg-config perl-core perl-IPC-Cmd perl-Text-Template \
  autoconf automake libtool python3 python3-pip \
  zlib-devel openssl-devel libcurl-devel libxml2-devel
```

这些步骤关闭 WebSocket/Rust、Python UDF、分析模块及 Benchmark/dump 工具，只构建原生连接和数据重整所需的组件。若业务需要这些可选组件，应另行启用、编译并验证对应运行时，不能认为核心部署包已经包含它们。

## 2. 路径、版本与日志约定

以下命令在同一个 Bash 会话中顺序执行。其他机器只需修改第一行的源码路径。构建目录放在源码目录之外，避免把构建产物混入源码提交。

```bash
export TD_SRC=/home/luhd/TSDB-DEV/TDengine
export TD_BUILD="$(dirname "$TD_SRC")/tdengine-build-compact-release"
export TD_LOG="$TD_BUILD/validation"
export TD_VERSION=3.4.3.9
export TD_COMPAT_VERSION=3.0.0.0
export TD_PACKAGE_TYPE=preRelease
export TD_JOBS=4

cd "$TD_SRC"
mkdir -p "$TD_LOG"
set -o pipefail

cmake --version
gcc --version
g++ --version
uname -m
git rev-parse HEAD | tee "$TD_LOG/source-commit.txt"
git status --short | tee "$TD_LOG/source-status.txt"
```

`pipefail` 使下面的 `命令 | tee 日志` 在编译失败时仍返回失败状态。内存不足时把 `TD_JOBS` 调小；Debug 和 Release 使用不同构建目录。

**版本号需要同时传给 CMake 和打包脚本。** 当前源码的默认版本是 `3.4.3.9.alpha`；`pack_community_tar.sh -n` 只接受 3～5 段数字版本，不能传入 `.alpha`。本文显式把编译版本设为 `3.4.3.9`，并用 `preRelease` 区分自建预发布包。打包参数不会重新编译或改变二进制版本；仅改压缩包文件名也不能改变版本。

自建版本包含本仓库的修改，不是未经修改的官方发行包。保留源码提交、未提交修改、构建参数和校验和，避免与同版本号的官方包混用。未提交的新文件也必须纳入源码归档，单独的 `git diff` 不能包含这些文件。

## 3. 配置 Release 构建

```bash
cmake -S "$TD_SRC" -B "$TD_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON \
  -DBUILD_ENTERPRISE=OFF \
  -DBUILD_VER_NUMBER="$TD_VERSION" \
  -DBUILD_VER_COMPATIBLE="$TD_COMPAT_VERSION" \
  -DBUILD_VER_TYPE="$TD_PACKAGE_TYPE" \
  -DBUILD_CONTRIB=ON \
  -DBUILD_USE_PUBLIC_DEPS=ON \
  -DBUILD_TEST=OFF \
  -DBUILD_DEPENDENCY_TESTS=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_WEBSOCKET=OFF \
  -DRUST_BINDINGS=OFF \
  -DBUILD_WITH_UDF=OFF \
  -DBUILD_PYUDF=OFF \
  -DBUILD_WITH_ANALYSIS=OFF \
  2>&1 | tee "$TD_LOG/configure.log"

cp "$TD_BUILD/CMakeCache.txt" "$TD_LOG/CMakeCache.snapshot.txt"
```

说明：

- `BUILD_ENTERPRISE=OFF` 使用社区版实现，不依赖相邻的企业源码仓库。
- `BUILD_CONTRIB=ON` 让 CMake 构建所需的第三方依赖；把它改为 OFF 不是所有依赖缺失问题的通用解决方法。
- 本仓库默认使用 `deps/` 中对应架构的 RocksDB 预编译库。若目标架构或 ABI 不匹配，需在配置时选择 `-DBUILD_ROCKSDB=ON -DROCKSDB_USE_DEPS=OFF` 从源码构建，并重新验证目标环境。
- `CMAKE_BUILD_RPATH_USE_ORIGIN=ON` 尽量将构建树内部库引用改为相对路径。运行构建目录中的程序时，仍按下文显式设置 `LD_LIBRARY_PATH`。
- `BUILD_TOOLS=OFF` 关闭的是 Benchmark/dump 等工具；当前源码仍定义 `shell` 目标，可单独构建命令行客户端。
- 关闭 UDF 后，运行实例的 `taos.cfg` 应配置 `udf 0`。需要 C UDF 时改为 `BUILD_WITH_UDF=ON`，再显式构建 `taosudf`；Python UDF 还需要单独的 SDK/运行时配置。

第三方文件缓存位于源码目录下的 `.externals/` 等目录，不完全位于 `TD_BUILD` 内。不要同时运行多个共享此依赖目录的首次构建。

## 4. 分别编译服务端、客户端库和命令行客户端

### 4.1 服务端

```bash
cmake --build "$TD_BUILD" --target taosd -j "$TD_JOBS" \
  2>&1 | tee "$TD_LOG/build-server.log"

test -x "$TD_BUILD/build/bin/taosd"
"$TD_BUILD/build/bin/taosd" -V
```

产物为 `TD_BUILD/build/bin/taosd`。显式指定目标不会把可选的 UDF、Benchmark、dump 等全部编出来。

### 4.2 原生 C 客户端库

```bash
cmake --build "$TD_BUILD" --target taos -j "$TD_JOBS" \
  2>&1 | tee "$TD_LOG/build-client-libraries.log"

test -f "$TD_BUILD/build/lib/libtaos.so"
test -f "$TD_BUILD/build/lib/libtaosnative.so"
```

必须一起发布以下两个库：

| 文件 | 用途 |
| --- | --- |
| `build/lib/libtaos.so` | 对外 C API / 驱动分发层 |
| `build/lib/libtaosnative.so` | 原生 SQL 解析、执行与连接实现，包含本次修改 |

只替换 `libtaos.so`，保留旧的 `libtaosnative.so`，不能完成此次客户端升级。Python `taospy` 等通过 C 客户端访问的程序，也要加载这组配套库。

### 4.3 命令行客户端

```bash
cmake --build "$TD_BUILD" --target shell -j "$TD_JOBS" \
  2>&1 | tee "$TD_LOG/build-shell.log"

export LD_LIBRARY_PATH="$TD_BUILD/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
test -x "$TD_BUILD/build/bin/taos"
"$TD_BUILD/build/bin/taos" -V
```

`--target taos` 生成库，**`--target shell` 才生成命令行程序**。当前 CMake 先生成 `build/bin/taosql`，再复制为 `build/bin/taos`；包内使用后者。

也可以一次编译三个目标：

```bash
cmake --build "$TD_BUILD" --target taosd taos shell -j "$TD_JOBS"
```

若启用了 C UDF，还需执行：

```bash
cmake --build "$TD_BUILD" --target taosudf -j "$TD_JOBS"
```

### 4.4 构建产物检查

```bash
file "$TD_BUILD/build/bin/taosd" "$TD_BUILD/build/bin/taos" \
  "$TD_BUILD/build/lib/libtaos.so" "$TD_BUILD/build/lib/libtaosnative.so"

ldd "$TD_BUILD/build/bin/taosd" | tee "$TD_LOG/ldd-server.txt"
ldd "$TD_BUILD/build/bin/taos" | tee "$TD_LOG/ldd-shell.txt"
ldd "$TD_BUILD/build/lib/libtaosnative.so" | tee "$TD_LOG/ldd-native.txt"

"$TD_BUILD/build/bin/taosd" -V | tee "$TD_LOG/server-version.txt"
"$TD_BUILD/build/bin/taos" -V | tee "$TD_LOG/client-version.txt"
```

确认架构、版本及提交信息正确，`ldd` 没有 `not found`，客户端的核心库路径指向本次构建目录。以上版本命令不启动数据库服务。

## 5. 运行隔离集成测试

使用新的 Python 环境；测试会自行创建临时数据目录、选择本地端口、启动自己的 taosd，并进行取消和 SIGKILL 恢复测试。

```bash
python3 -m venv "$TD_BUILD/test-venv"
"$TD_BUILD/test-venv/bin/python" -m pip install taospy

TDENGINE_BUILD_DIR="$TD_BUILD" \
LD_LIBRARY_PATH="$TD_BUILD/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
"$TD_BUILD/test-venv/bin/python" -u \
  "$TD_SRC/test/compact/test_community_compact.py" -v \
  2>&1 | tee "$TD_LOG/compact-integration.log"
```

期望结果是 8 个测试全部通过并输出 `OK`。日志会打印临时实例目录，可据此检查 `stdout.log`、服务端日志和 CURRENT。测试不会连接现有数据库；无需先安装系统服务或启动系统 taosd。

## 6. 制作服务端、客户端部署包

使用当前仓库的 [packaging/pack_community_tar.sh](packaging/pack_community_tar.sh)。此脚本根据本机 `uname -m` 决定包名架构，适用于本机构建；不能仅通过修改包名实现交叉编译。

### 6.1 检查必需产物

```bash
for td_file in \
  "$TD_BUILD/build/bin/taosd" \
  "$TD_BUILD/build/bin/taos" \
  "$TD_BUILD/build/lib/libtaos.so" \
  "$TD_BUILD/build/lib/libtaosnative.so"; do
  test -f "$td_file" || { echo "缺少产物: $td_file" >&2; exit 1; }
done

bash "$TD_SRC/packaging/pack_community_tar.sh" -h
```

脚本会同时生成两个包，因此即使只准备分发客户端包，调用脚本的构建目录中也必须有 taosd。脚本只收集已有产物，不负责补编译缺失组件。

### 6.2 打包

```bash
cd "$TD_SRC"
bash packaging/pack_community_tar.sh \
  -c "$TD_BUILD" \
  -n "$TD_VERSION" \
  -m "$TD_COMPAT_VERSION" \
  -V "$TD_PACKAGE_TYPE" \
  2>&1 | tee "$TD_LOG/package.log"
```

`-c` 指向 CMake 构建根目录 `TD_BUILD`，不是其下的 `build/`。输出固定写到源码根目录下的 `release/`。本示例在 x86_64 上生成：

```text
release/TDengine-server-3.4.3.9-preRelease-Linux-x64.tar.xz
release/TDengine-client-3.4.3.9-preRelease-Linux-x64.tar.xz
```

aarch64 的架构后缀为 `arm64`。`-V stable` 的文件名不带 `-stable`；`beta` / `preRelease` 带相应后缀。再次执行会重建对应包目录和同名包，需保留旧包时先归档到另一个目录。

服务端包包含 taosd、taos、两个核心驱动库、配置、头文件、C 示例及安装脚本；客户端包包含 taos、两个驱动库、配置、头文件、C 示例及客户端安装脚本。存在 `taosudf`、`taosBenchmark` 或 `taosdump` 产物时，脚本会按其规则收集。该社区版脚本不收集 taosadapter、taoskeeper、Explorer，也不收集 WebSocket 驱动；本文使用原生连接。

脚本输出文字中个别地方仍写着 `tar.gz`，但实际创建的是 **xz 压缩的 `.tar.xz`**，应使用 `tar -tJf` / `tar -xJf`。不要沿用旧示例中的 `tar -xzf`。

### 6.3 清单与完整性检查

下面按本示例的预发布包命名；若修改了版本、类型或架构，同步修改变量。

```bash
export TD_SERVER_PKG="$TD_SRC/release/TDengine-server-$TD_VERSION-$TD_PACKAGE_TYPE-Linux-x64.tar.xz"
export TD_CLIENT_PKG="$TD_SRC/release/TDengine-client-$TD_VERSION-$TD_PACKAGE_TYPE-Linux-x64.tar.xz"

xz -t "$TD_SERVER_PKG"
xz -t "$TD_CLIENT_PKG"
tar -tJf "$TD_SERVER_PKG" | tee "$TD_LOG/server-package-files.txt"
tar -tJf "$TD_CLIENT_PKG" | tee "$TD_LOG/client-package-files.txt"

(
  cd "$TD_SRC/release"
  sha256sum "$(basename "$TD_SERVER_PKG")" "$(basename "$TD_CLIENT_PKG")" \
    > "SHA256SUMS-compact-$TD_VERSION-$TD_PACKAGE_TYPE.txt"
)
```

服务端清单应有 `TDengine-server-3.4.3.9/bin/taosd`、`bin/taos`、`driver/libtaos.so.3.4.3.9`、`driver/libtaosnative.so.3.4.3.9`、`driver/vercomp.txt` 和 `install.sh`。客户端清单应有相应驱动及 `install_client.sh`。压缩包名有预发布/系统/架构后缀，内部顶层目录没有这些后缀。

### 6.4 在部署机检查动态库

把包复制到目标机器后先验证校验和、解压并检查依赖。以下过程只在临时目录检查文件，不安装服务：

```bash
sha256sum -c SHA256SUMS-compact-3.4.3.9-preRelease.txt

export TD_VERIFY_DIR="$(mktemp -d /tmp/tdengine-package-check.XXXXXX)"
tar -xJf TDengine-server-3.4.3.9-preRelease-Linux-x64.tar.xz -C "$TD_VERIFY_DIR"
export TD_UNPACKED="$TD_VERIFY_DIR/TDengine-server-3.4.3.9"

# 未安装时，包内驱动只有带版本号的真实文件；补齐临时目录中的运行别名。
ln -s libtaos.so.3.4.3.9 "$TD_UNPACKED/driver/libtaos.so"
ln -s libtaosnative.so.3.4.3.9 "$TD_UNPACKED/driver/libtaosnative.so"

LD_LIBRARY_PATH="$TD_UNPACKED/driver" ldd "$TD_UNPACKED/bin/taos"
ldd "$TD_UNPACKED/bin/taosd"
LD_LIBRARY_PATH="$TD_UNPACKED/driver" "$TD_UNPACKED/bin/taos" -V
"$TD_UNPACKED/bin/taosd" -V
```

检查两个核心库确实解析到临时目录的 `driver/`，而不是构建机绝对路径或目标机器残留的旧库。上述校验和文件包含两个包，运行 `sha256sum -c` 的目录应同时有两个包。

## 7. 在服务端机器安装或升级

本节使用 root/systemd 安装方式。服务端包已包含客户端，在服务端机器上使用服务端安装脚本即可。

升级现有实例前，记录实际二进制、配置、数据目录和版本；按现有备份流程完成可恢复备份，并安排服务停机。安装脚本会操作已有程序文件和服务，不能把安装过程当作只读检查。

```bash
command -v taosd
command -v taos
taosd -V
taos -V
sudo systemctl cat taosd
sudo systemctl stop taosd

# 另存配置示例；数据备份应按实际数据目录及备份流程另行完成。
sudo tar -czf /var/tmp/taos-config-before-compact-upgrade.tar.gz /etc/taos

tar -xJf TDengine-server-3.4.3.9-preRelease-Linux-x64.tar.xz
cd TDengine-server-3.4.3.9
sudo bash ./install.sh -s -e no
sudo ldconfig
```

首次安装不需要前面的旧版本检查、停止旧服务和旧配置备份命令，直接从解压、安装开始。

root 默认程序目录为 `/usr/local/taos`，配置目录 `/etc/taos`，数据和日志目录分别为 `/var/lib/taos`、`/var/log/taos`；以实际已有配置为准。安装后核对配置，已有集群保留原来的 FQDN、firstEp、数据目录和端口，不要套用新单节点配置覆盖它们。

对于一个新的单节点实例，在 `/etc/taos/taos.cfg` 中设置实际可解析的节点名，例如：

```text
fqdn db01.example.com
firstEp db01.example.com:6030
serverPort 6030
dataDir /var/lib/taos
logDir /var/log/taos
udf 0
```

```bash
getent hosts db01.example.com
sudo systemctl daemon-reload
sudo systemctl start taosd
sudo systemctl status taosd --no-pager
sudo journalctl -u taosd -n 100 --no-pager
```

将示例主机名替换为实际主机名，确保服务端与客户端均能解析。多节点部署先在测试环境验证升级顺序和行为；当前重整改动尚未完成多副本专项验证。

升级后检查：

```bash
/usr/local/taos/bin/taosd -V
/usr/local/taos/bin/taos -V
readlink -f /usr/lib/libtaos.so
readlink -f /usr/lib/libtaosnative.so
ldd /usr/local/taos/bin/taos
```

确认驱动链接指向本次安装的版本。Python、Java JNI 或其他长期运行的原生客户端进程需要重启，才能加载替换后的动态库。

## 8. 在纯客户端机器安装

客户端安装脚本有自己的固定系统安装路径，没有服务端脚本的 `-d` / `-s` 参数。已安装 taosd 的机器会被客户端脚本直接跳过，所以服务端机器应使用上一节的服务端包升级。

```bash
tar -xJf TDengine-client-3.4.3.9-preRelease-Linux-x64.tar.xz
cd TDengine-client-3.4.3.9
sudo bash ./install_client.sh
sudo ldconfig

/usr/local/taos/bin/taos -V
readlink -f /usr/lib/libtaos.so
readlink -f /usr/lib/libtaosnative.so
ldd /usr/local/taos/bin/taos
```

在客户端的 `taos.cfg` 中设置能访问的 firstEp，或在命令行指定服务端地址。`-p` 后不跟明文密码，由客户端交互读取：

```bash
taos -c /etc/taos -h db01.example.com -P 6030 -u root -p
```

## 9. 部署后的功能验收

连接到目标服务后，先检查版本与节点状态，再在独立验收库中执行：

```sql
SELECT SERVER_VERSION(), CLIENT_VERSION();
SHOW DNODES;

CREATE DATABASE compact_acceptance VGROUPS 1 BUFFER 16;
CREATE TABLE compact_acceptance.t (ts TIMESTAMP, v INT);
INSERT INTO compact_acceptance.t VALUES ('2026-01-01 00:00:00', 1)
                                       ('2026-01-01 00:00:01', 2);
FLUSH DATABASE compact_acceptance;
DELETE FROM compact_acceptance.t WHERE ts = '2026-01-01 00:00:00';
COMPACT DATABASE compact_acceptance FORCE;
SHOW COMPACTS;
-- 将返回的任务 ID 填入：SHOW COMPACT <任务ID>;
SELECT * FROM compact_acceptance.t;
```

等待任务从 `SHOW COMPACTS` 消失，确认查询仅保留 `v=2` 的一行、服务端日志没有重整错误。验收完再删除验收库：

```sql
DROP DATABASE compact_acceptance;
```

上述日期适用于当前版本的验收示例；在较晚时间运行时，请改成数据库 KEEP 范围内的时间。META_ONLY 的运行方式和边界见 [COMPACTION.md](COMPACTION.md)，不要把它作为在线业务库的无影响操作。

## 10. 常见构建和打包问题

### 10.1 `termio.h: No such file or directory`

本次 Fedora 44 环境只有 `termios.h`；`tools/shell/src/shellArguments.c` 在普通 Linux 分支仍包含 `termio.h`。服务端和客户端库可以成功构建，shell 会在此处失败。

本次验证使用构建目录内的兼容头文件，不修改系统头文件或业务源码。遇到同一错误时，在配置/构建会话中执行：

```bash
mkdir -p "$TD_BUILD/compat-include"
cat > "$TD_BUILD/compat-include/termio.h" <<'HEADER'
#include <termios.h>
#include <sys/ioctl.h>
HEADER
export C_INCLUDE_PATH="$TD_BUILD/compat-include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
cmake --build "$TD_BUILD" --target shell -j "$TD_JOBS"
```

这适用于当前 shell 文件未直接使用旧 `struct termio` API 的情况。其他发行版若原本提供 `termio.h`，不需要该兼容步骤。

### 10.2 Perl 模块或第三方头文件缺失

- OpenSSL 配置阶段报告 `FindBin.pm`、`Params/Check.pm`、`ExtUtils/MM/Utils.pm` 缺失：先补齐完整 Perl 环境。Fedora 上使用前述 `perl-core` 和 `perl-IPC-Cmd`；不要只安装最小 perl-interpreter。
- `gsasl.h` / `curl/curl.h` 缺失：检查 `BUILD_CONTRIB=ON`，以及对应 ExternalProject 的构建日志。可先构建 `ext_gsasl` / `ext_curl` 目标定位问题；依赖成功后再构建主目标。
- `aclocal-1.16: command not found`：这是依赖源码试图重生成 autotools 文件的错误，不是 COMPACT 源码编译错误。优先使用匹配的构建工具链。本次环境也遇到过解包生成文件的时间戳导致不必要重生成的问题，见下一段的有限处理方法。

仅当错误出现在已经成功 configure 的 `ext_gsasl`、未修改其 `.m4` / `configure.ac`、且只是生成文件时间戳异常时，可恢复已生成文件的顺序后重试：

```bash
TD_GSASL_SOURCE="$TD_SRC/.externals/build/ext_gsasl/src/ext_gsasl" python3 - <<'PY'
import os
from pathlib import Path
import time

root = Path(os.environ['TD_GSASL_SOURCE'])
assert root.is_dir(), root
now = time.time()
for name in ('aclocal.m4', 'configure'):
    path = root / name
    if path.exists():
        os.utime(path, (now - 10, now - 10))
for path in root.rglob('Makefile.in'):
    os.utime(path, (now - 8, now - 8))
for path in root.rglob('Makefile'):
    os.utime(path, (now - 1, now - 1))
PY
cmake --build "$TD_BUILD" --target ext_gsasl -j "$TD_JOBS"
```

如果实际修改过 autotools 输入文件，应使用匹配的 autotools 正常重新生成，不能采用这一处理方法。

### 10.3 包内没有客户端或版本不一致

- `required binary not found: .../bin/taos`：补编译 `shell` 目标。
- `invalid version format ...alpha`：按第 2、3 节设置数字编译版本，然后重新编译；不要仅给旧二进制套数字包名作为正式交付。
- 仍提示 COMPACT 不支持：检查正在执行的 `taos` 路径，以及实际加载的 `libtaosnative.so` 是否来自新构建。系统库、`LD_LIBRARY_PATH` 和已启动进程都可能继续使用旧库。
- `GLIBC_*` / `GLIBCXX_* not found`：回到与部署环境兼容的构建系统重新构建；复制单个新版 glibc 到部署机器不是本文的升级方式。

### 10.4 其他打包入口

本社区版核心部署使用 `pack_community_tar.sh`。当前 `packaging/tools/makepkg.sh`、`makeclient.sh` 是包含更多组件的旧入口；`packaging/deb/makedeb.sh`、`packaging/rpm/makerpm.sh` 还会查找相邻企业 packaging 和 taosx 目录。不能在只有本开源仓库时，把这些脚本当成已验证可直接运行的 DEB/RPM 发布流程。若交付必须为 DEB/RPM，需要先适配其依赖和载荷，再做对应安装/升级验证。

`cmake --install` 会调用仓库自带的系统安装脚本；本文没有把 `DESTDIR` 或 `--prefix` 当作已经验证的无副作用打包暂存方案。

## 11. 本次验证记录

2026-11-03，在当前 Fedora Linux x86_64 环境完成：

- 服务端 `taosd`、客户端库 `taos` 和命令行 `shell` 的构建；shell 使用第 10.1 节兼容头文件。
- 数据重整 8 组隔离集成测试及后续针对性回归。
- 用已有 Debug / `3.4.3.9.alpha.community` 产物执行社区脚本，生成数字版本标记为 `3.4.3.9` 的 `preRelease` 服务端与客户端包；此操作验证打包结构，不代表已执行第 3 节的全新 Release 构建。
- 检查生成包的必需文件、xz 完整性，以及解压后从包内驱动目录加载客户端并显示版本。

本次验证包位于仓库 `release/TDengine-{server,client}-3.4.3.9-preRelease-Linux-x64.tar.xz`，仅用于本地验证。正式交付应从第 2 节开始，使源码、编译版本、构建类型和包版本一致，并在目标操作系统完成依赖检查、安装和功能验收。本文中的 root 安装、系统服务升级及跨发行版部署尚未在本机执行。
