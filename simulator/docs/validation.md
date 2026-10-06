# 模拟器整合验证

验证时间为 2026-10-06 至 2026-10-07，源码基线为 `560ff9ab26bf76293228c43b31fb42eae7b3e76e`。宿主为 Apple Silicon macOS，QEMU 11.1.2、Python 3.14、Podman Linux 容器；官方核心固定为 2.9.12 / 序列 30，应用索引序列 179。

以下结果来自实际执行。集成测试使用独立客体及临时磁盘，未使用个人账号；镜像、完整日志和自动生成报告保留在本地忽略目录 `build/verification/`，不随源码提交。

| 检查 | 结果和覆盖范围 |
| --- | --- |
| 最新核心源码 | `make -j2 host-test all` 通过，生成四个静态 MIPS ELF；`examples/hello` 编译与 ABI 检查通过 |
| Go 源码 | `App/c1device`、`App/book-reader`、`App/music-player` 的 `go test ./...` 与 `go vet ./...` 均通过；阅读器和音乐播放器交叉编译通过 |
| 本地核心入口 | `core_test.py` 通过：实际调用 `--core-dir`、核对四个客体 ELF、七个应用的屏幕与按键、原版终端字符输入、开发盘选择、恢复官方版本及保留测试数据 |
| 最新应用源码运行 | 自编译阅读器实际打开书籍、翻页、保存阅读进度并重开；自编译音乐播放器实际播放、暂停、恢复、保存音量并重开，录制到非零 QEMU 声卡 PCM |
| 离线音频 | `audio_test.py` 18 项检查通过：原版播放器与 Pinao、MIPS MP3/FLAC 解码、音量、暂停/停止及声卡输出 |
| 在线音频 | `online_audio_test.py` 10 项检查通过：HTTP MP3、HTTPS FLAC/AAC、请求头、重定向、增益、证书与错误处理 |
| 按需商店下载 | `repository_cache_test.py` 10 项通过；`online_repository_test.py` 实际安装 Pelican 并从缓存离线重装，安装 ELF 与官方包一致 |
| 数字键 | `netease_keyboard_test.py`、`flomo_keyboard_test.py` 通过：实际账号框、电脑数字键和网页按钮、返回终端后的映射；未提交登录 |
| Cookie 导入 | `cookie_import_test.py` 用虚构凭据验证格式、权限、错误处理、日志与重启持久化，通过 |
| Mail 缺字 | `mail_font_test.py` 通过：原版 0.1.0、原字体备份、MiSans 及许可哈希、原始 ELF 保持不变、重复修复、恢复原画面；替换后的中文画面已目视核对 |

核心主机测试发现并处理了两项容器要求：需要宿主架构的 curl，且 Bash 模块测试需要可执行的 `/dev/shm`。构建镜像和 `build_core.py` 已包含这两项设置，未为此修改核心业务代码。

Mail 验证还复现了切换应用时的退出竞态：Go 应用返回桌面的回调会向当前父进程发送 SIGKILL，而被桥接进程收养的孤儿进程可能误杀桥接进程。退出流程现在先记录子进程句柄，再暂时关闭孤儿收养，按原有流程回收应用。修复后重新执行核心、18 项音频、网易云和 flomo 键盘测试，以及 Mail 的多次切换与字体回滚，均通过。

## 产物核对

本地核心运行时核对的 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| C1ancher | `2114762fa674dea087f4d37c7bfc600ce101870f1b423c7eab950f913162db00` |
| C1ancher-launcher | `3085ac7f67dc4c254d557ecd1b56c2b5667e2a6d64ee0cf6a4d268ece886bed6` |
| c1pkg | `cecd410fc68ab81b0260566d7857176c8a54926dc1bf3a77d20ee823c88d76b7` |
| c1updater | `c4cb3d89a5e972221ae939f610e553256b3f79bd5ab75f09d81e6814dbb7093d` |

Mail 原始 ELF 为 `cf4b465e36ea75e1d3c0a59a0604b51638aa50338fd87eee9e84fe7ceca7acfe`；修复前后均一致。Mail 空列表修复后的 framebuffer 为 `59778b595ecd0b0956a9e7cfc56791d2a5ab94421c3b82a1e2a18bedf2374062`，作为已确认中文画面的回归基准。

## 验证边界

Windows/Linux 宿主尚未实际执行。Malta 与原机 SoC 不同；真实电子纸时序、无线和 USB 硬件、休眠耗电、厂商启动链及整机升级仍需实机验证。QEMU 客体终端使用 BusyBox `sh` 回退，未集成原机 Bash 与中文输入法；构建容器里的 Bash 主机测试不能证明这些功能已在客体实现。

上述应用验证不包含真实账号登录、发邮件、支付或所有网络服务功能。官方签名版本和本地编译版本分别核对，不将本地构建声明为官方签名发布。
