# 在 Ubuntu ARM64 上构建 TDengine 社区版

默认环境是 Ubuntu 24.04 LTS、ARM64（`aarch64`）。脚本构建数据重整所需的 `taosd`、`taos`、`libtaos.so` 和 `libtaosnative.so`。运行镜像提供原生连接端口 `6030/tcp`。

以下命令从 TDengine 源码根目录执行。

## 原生编译

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  autoconf automake build-essential ca-certificates cmake curl git libcurl4-openssl-dev \
  libssl-dev libtext-template-perl libtool libxml2-dev perl pkg-config python3 xz-utils zlib1g-dev

packaging/ubuntu-arm64/build.sh ../tdengine-build-ubuntu-arm64
```

产物在指定目录的 `build/bin/` 和 `build/lib/`。脚本检查 ELF 架构、动态库依赖，以及服务端和客户端的版本。构建使用仓库中的 ARM64 RocksDB 静态库。需要从源码编译 RocksDB 时，设置 `TD_BUILD_ROCKSDB=ON`。可通过 `TD_JOBS` 和 `TD_VERSION` 调整并行度和数字版本。

## 构建和运行镜像

```bash
packaging/ubuntu-arm64/build-image.sh
```

默认镜像标签为 `tdengine-community:3.4.3.9-ubuntu24.04-arm64`，可以通过脚本的第一个参数自定义。需要 Docker Engine 和 Buildx。使用 Podman 时设置 `CONTAINER_ENGINE=podman`。在 x86_64 上构建需要支持 ARM64 的远程构建节点或 QEMU 仿真，仿真编译耗时更长。

脚本使用当前工作树中由 Git 跟踪的文件及未被忽略的新文件，镜像标签 `org.opencontainers.image.revision` 记录工作树的基准提交。镜像的构建和运行阶段都使用 Ubuntu 24.04。运行阶段以 UID/GID 10001 启动前台 `taosd`，配置在 `/etc/taos/taos.cfg`，数据和日志分别写入 `/var/lib/taos`、`/var/log/taos`。

```bash
docker run -d --name tdengine --hostname tdengine \
  -p 6030:6030/tcp \
  -v tdengine-data:/var/lib/taos \
  -v tdengine-logs:/var/log/taos \
  tdengine-community:3.4.3.9-ubuntu24.04-arm64

docker exec tdengine taos -s 'SHOW DNODES;'
```

保留数据卷时也应保留相同的主机名。容器外的原生客户端必须能解析服务端返回的 FQDN。可将 `--hostname` 和 `TAOS_FQDN` 指向部署环境的实际域名，并为容器和客户端配置解析。绑定宿主机目录时，确保容器内的 UID 10001 对数据及日志目录有写权限。

导出镜像后可复制到目标机离线导入：

```bash
docker save -o tdengine-3.4.3.9-ubuntu24.04-arm64.tar \
  tdengine-community:3.4.3.9-ubuntu24.04-arm64
docker load -i tdengine-3.4.3.9-ubuntu24.04-arm64.tar
```
