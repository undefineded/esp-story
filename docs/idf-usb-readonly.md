# 原生 USB 只读文件接口

这是迁移诊断能力，不代表完整旧版文件管理恢复。当前桌面页面检查 `fs` 特征，
因此不会自动开放此只读模式；不得为了绕过该检查宣称已支持完整 `fs`。
绑定鉴权、删除、重命名、重扫与 NFC 路由仍未接入。
上传是独立的默认关闭诊断选项，详见下文；前半部分描述默认只读配置。

## 启用条件

使用 `sdkconfig.usb-diagnostics`，SD 成功挂载且音频诊断初始化后，hello 返回
`features: ["fs_readonly"]`。启动早期或挂载失败时特征列表为空。
协议 JSON 版本为 4，使用现有 `SBX4` 二进制帧、头 CRC 和 JSON+raw 载荷 CRC；
不是换行 JSON 串口协议。普通 IDF 控制台保持关闭。

## 命令

所有请求包含 `v: 4`、`cmd: "fs"`，建议携带整数 `id`。

| act | 参数 | 说明 |
| --- | --- | --- |
| list | path、offset、limit | 无需会话；默认根目录；返回 data.entries/total/returned |
| session_begin | 无 | 独占存储；音频未释放文件返回 AUDIO_BUSY |
| read_begin | path | 返回 data.xid/size/chunk/name/path，chunk 固定 4096 |
| read_chunk | xid、offset | 返回原始二进制载荷及 data.len/offset/size/eof |
| read_end | xid | 关闭当前下载，不结束会话 |
| cancel | xid | 关闭当前下载，不结束会话 |
| session_end | 无 | 有未关闭下载则返回 BUSY；否则释放存储 |

下载前长按诊断 SET 停止播放。暂停不释放音频句柄，不能启动下载会话。
结束会话后不会自动恢复播放。重复 read_chunk 会定位到同一偏移重新读取。
在 EOF 偏移读取成功，raw 长度为 0。一次仅允许一个下载句柄。

## 边界及恢复

- JSON 请求最多 1024 字节、16 层；响应 JSON 最多 4096 字节、raw 最多 4096 字节。
- 路径拒绝 `..`、反斜杠、控制字符和转义 NUL；主机测试同时拒绝符号链接。
  文件根目录由应用提供，不接受请求修改。
- 目录最多检查 4096 个目录项，超过返回 LIMIT_EXCEEDED；每页最多 16 项，
  长文件名可能进一步缩短页。客户端必须按 returned 推进 offset，直到 total，
  不可假设请求的 limit 等于实际返回数量。容量/剩余空间尚未返回。
- 固件文件大小受 `LONG_MAX` 限制（ESP32 通常约 2 GiB），超过返回 TOO_LARGE。
- 下载 30 秒无有效读取会关闭文件；会话 60 秒无有效操作释放独占权限。
- 队列满、内存分配失败或主机不接收时可能无响应/帧不完整；客户端必须有超时。
  块读取可重试；会话开始/读开始 ACK 丢失时可能返回 BUSY，需结束/等待超时恢复。
- 上传重复块校验相同字节，拒绝同偏移同长度但不同内容。

## 验证

`tools/test_host.sh` 包含文件租约、路径、分块、重试、截断、空文件和超时测试。
`IDF_PATH=/path/to/esp-idf bash tools/test_native_protocol.sh` 使用真实 cJSON 与协议实现，
验证文件命令、长名称分页和带二进制 NUL 的 SBX4 CRC 帧往返。
这些不替代真实 USB 连接、SD 拔插、断电恢复或吞吐验收。

## 可选上传诊断

`CONFIG_STORYBOX_USB_FILE_WRITES=y` 启用上传，默认关闭。hello 此时声明
`fs_transfer` 而不是 `fs_readonly`，仍不声明完整 `fs`。CI 的仅编译配置覆盖此分支。
上传同样要求 session_begin 成功；只允许已存在的父目录，不自动创建目录。
禁止覆盖索引内部文件及名称中包含 `.storybox-` 的恢复文件。

| act | 参数 | 结果 |
| --- | --- | --- |
| write_begin | path、size、overwrite（默认 true） | 返回 xid/size/chunk/path；不会截断原文件 |
| write_chunk | xid、offset，附带 1～4096 字节 raw | 连续写入；立即重复块必须逐字节一致 |
| write_end | xid | 完整接收并同步后替换目标；返回 path/size/expected/complete |
| cancel | xid | 关闭上传并删除本次创建的临时文件，原文件不变 |
| recover | path | 显式恢复中断事务，不删除恢复数据 |

write_end 成功后的应答丢失，可以在同一会话、未开始下一次上传时重试。
未完成上传返回 INCOMPLETE，不发布目标文件。会话 60 秒无有效操作会取消上传。
恢复将遗留 `.storybox-upload` 改名为 `.storybox-interrupted`；仅当目标缺失时将
`.storybox-backup` 恢复为目标。目标和备份同时存在、或隔离文件已存在时返回
RECOVERY_REQUIRED，需人工检查。不会自动选择新旧版本或覆盖隔离数据。

上传/恢复前持久化索引脏标记。会话结束后诊断任务自动刷新媒体目录、播放列表和索引，
不会自动开始播放；刷新失败保留旧快照并重试。重启发现脏标记会忽略缓存重新扫描。
本模式用于文件事务诊断，不可宣称已恢复完整桌面管理体验或作为产品固件发布。
主机测试注入写入、flush/fsync/close、重命名及回滚失败，检查旧文件/备份仍可恢复。
