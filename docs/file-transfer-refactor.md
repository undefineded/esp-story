# IDF USB 文件传输

产品 USB 配置通过 SBX4 二进制帧传输 JSON 元数据和原始文件数据：帧头包含 magic、长度、头 CRC32 和载荷 CRC32，单块原始数据最多 4096 字节。固件实现见 `components/hostlink/`，桌面端使用前应根据 hello 返回的能力判断兼容性。

`sdkconfig.usb-product` 启用产品 USB。当前 `fs_transfer` 支持目录分页、`session_begin` / `session_end`、文件读写、取消和恢复。目录列表每页最多 16 项；文件读写需显式会话。存储被音频或视频占用时，获取会话可能返回 `AUDIO_BUSY`，应先停止播放并等待资源释放。当前原生路由不实现旧文档中的 mkdir、remove、rename 或 fs rescan，不应将桌面端所有文件管理按钮视为已支持。

上传写同目录临时文件，结束时检查大小并以重命名提交；覆盖时使用备份，异常恢复由 `recover` 处理。FAT 多次重命名并非断电原子事务。上传修改会使媒体索引失效；不应假定每次写入后立即完成索引扫描。

只读诊断配置见 [idf-usb-readonly.md](idf-usb-readonly.md)。协议回归使用 `IDF_PATH=/path/to/esp-idf bash tools/test_native_protocol.sh`；真实设备须验证拔线、空间不足、覆盖恢复、大文件完整性及与播放的互斥。
