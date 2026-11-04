# 视频播放

当前固件使用乐鑫 `esp_player` 播放 MJPEG AVI 和内嵌 MP3。格式、转换命令、任务架构、统计及验收统一见 [IDF 视频播放器](idf-video-player.md)。

旧 Arduino 版本使用的 QOI AVI、独立音轨和按帧索引方案不适用于当前播放器；旧素材需从原视频重新转换。
