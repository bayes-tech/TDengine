# 开源版数据重整

本实现基于当前 3.4.3.9.alpha.community 源码已有的 SQL、RPC、mnode 事务、文件集迭代器与原子 CURRENT 切换机制补齐 COMPACT；没有引入企业版插件源码或更改企业版的重整入口。

功能语义参考[官方数据重整说明](https://docs.taosdata.com/operations-and-tooling/operations/maintenance/#数据重整)。

## 使用

```sql
COMPACT DATABASE power;
COMPACT DATABASE power START WITH '2026-01-01' END WITH '2026-02-01' FORCE;
COMPACT power.VGROUPS IN (2, 3);
USE power;
COMPACT VGROUPS IN (2);
COMPACT DATABASE power META_ONLY;
SHOW COMPACTS;
SHOW COMPACT 123456;
KILL COMPACT 123456;
KILL COMPACT 123456 FORCE;
```

- 请求通过已有数据库权限检查；vgroup 必须属于指定数据库，重复 ID、无效 ID、反向时间范围会被拒绝。
- 数据重整先等待刷盘，再向后台提交任务，返回任务 ID。时间范围选择与范围相交的完整文件集，不删除范围以外的有效行。
- 重写数据、索引、统计和 STT，合并相同主键的版本，过滤已删除行和已删除表。小表仍可能生成一个 STT；不保证所有表都转入 `.data`。
- 保留仍有效表的删除标记，防止重整输入快照以外的数据重新可见。后续写入的较新版本仍然可见。
- 同库的新请求等待上一个任务完成后提交；不同数据库可并行。进度由文件集完成数及其输入大小估算。
- 未变化的文件集跳过重写；`FORCE` 强制重写。CURRENT 中新增可选 `last compact cid`，识别重整期间并发提交的文件，避免下一次重整跳过它们。旧 CURRENT 无此字段时，首次重整会重新整理。
- 普通 KILL 设置取消标记，任务在安全检查点退出，清理未发布输出；已经原子提交的文件集继续有效。取消状态在响应成功前落盘。
- `KILL ... FORCE` 沿用现有 mnode 强制删除任务记录的行为，不等待 vnode 清理。节点离线时应按照官方说明谨慎使用。

## META_ONLY 的边界

META_ONLY 在 vnode 的串行写入队列中执行，持有元数据写锁，清除已删除表及已有表的过期 `table.db` 条目。保留当前条目、schema 历史和其他索引；存在尚未清理的元数据事务时重试，避免破坏事务回滚所需版本。

释放的 B-tree 页面供后续元数据写入复用，**不对元数据文件做操作系统级截断**。这条路径在处理完成后确认请求，不能承诺与企业版元数据文件重建实现相同的磁盘缩小幅度或返回延迟。执行前应按官方说明暂停该库的业务读写。普通数据重整不执行元数据清理。

## 恢复与错误处理

每个 vnode 在其根目录保存 `<tsdb-name>-compact.state`，以临时文件、fsync、rename 保存请求和终止状态。进程重启后，在 WAL 恢复完成后重新调度未完成的范围；重复请求按 ID 和开始时间去重。该状态文件不改变 SQL/RPC 协议。

调度、读取或输出失败不会算作成功完成；任务保持可见，进度轮询会重试。持续失败时检查服务端日志，使用普通 KILL 终止。CURRENT 切换失败沿用存储层的停机恢复策略，避免继续使用可能失配的文件句柄。

目前验证覆盖 Linux 单节点单副本。多节点复制、节点离线强制清理、快照迁移、Windows 和断电恢复仍需在对应环境执行专项验证；不能把进程崩溃测试等同于断电测试。

## 构建与验证

服务端、客户端库、命令行客户端、部署包制作及安装升级的详细步骤见 [BUILD_AND_DEPLOY.md](BUILD_AND_DEPLOY.md)。该文档也说明了 `taos` / `shell` 构建目标的区别、数字包版本与 `.alpha` 编译版本的关系，以及本次验证范围。

SQL 翻译入口也有修改，需同时重建并使用对应客户端库，旧开源客户端会在发起 RPC 前拒绝 COMPACT。

```sh
cmake -S . -B build-compact -DBUILD_TEST=OFF -DBUILD_TOOLS=OFF \
  -DBUILD_PYUDF=OFF -DBUILD_WITH_UDF=OFF -DBUILD_WITH_ANALYSIS=OFF -DRUST_BINDINGS=OFF
cmake --build build-compact --target taosd taos -j 6

# 在 Python 环境安装 taospy，然后运行独立集成测试。
# 测试自行启动临时目录和独立端口中的 taosd，不连接现有实例。
TDENGINE_BUILD_DIR="$PWD/build-compact" \
LD_LIBRARY_PATH="$PWD/build-compact/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
python test/compact/test_community_compact.py -v
```

测试覆盖数据回收、删除后重插、时间范围、vgroup 归属与重复 ID、FORCE、1200 次标签变更后的 META_ONLY、历史 schema、同库任务等待、取消、SIGKILL 恢复、主键、微秒/纳秒精度、并发提交和强制清理。文件集重整采用现有读写器和压缩配置，不新增存储格式版本。
