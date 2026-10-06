# 第三方来源与许可

本项目构建所需文件从上游下载到 `.cache/`，不把原设备个人数据或整机分区镜像作为输入。

| 组件 | 来源 | 许可/说明 |
| --- | --- | --- |
| C1auncher 2.9.12 及商店应用 | 官方签名核心通道 <https://www.fwz233.com/c1/core/v1/stable/manifest.v1>、应用索引 <https://www.fwz233.com/c1/v2/index.v1> | 自研源码 GPL v3；各原包及历史发行 ZIP 保留其字体、Go 及其他组件声明；核心及预装版本见 release.json，完整商店版本见已验证的应用索引 |
| 官方 2.0.0 安装包与历史许可材料 | <https://github.com/fwz233-RE/C1auncher/releases/tag/v2.0.0> | 提供本地信任公钥、附属资源及对应组件许可，原包校验后保留在缓存 |
| Linux 6.6.60 | <https://cdn.kernel.org/pub/linux/kernel/v6.x/> | GPL-2.0-only；源码压缩包包含 COPYING 与 LICENSES/ |
| BusyBox 1.37.0 | <https://busybox.net/downloads/> | GPL v2；源码压缩包包含 LICENSE |
| curl 8.11.0 | <https://curl.se/download.html> | curl license；源码压缩包包含 COPYING |
| FFmpeg 6.1.5 | <https://ffmpeg.org/releases/ffmpeg-6.1.5.tar.xz> | 本构建启用 version3，为 LGPL-3.0-or-later；MP3/FLAC/AAC 解码、音频重采样与 HTTP/HTTPS，未启用 GPL/nonfree 组件；OpenSSL 后端主机名/IP 校验由 guest/ffmpeg_tls.py 补齐 |
| OpenSSL 3 静态库 | Debian bookworm 的 mipsel `libssl-dev` 包，通过阿里云镜像及 APT 签名元数据下载 | Apache-2.0 及包内第三方声明；实际版本见 build/images/openssl-version，声明保留在 build/licenses/openssl/ |
| CA 根证书 | 构建容器的 Debian `ca-certificates` | 随包声明保留在 build/licenses/ca-certificates/ |
| Debian 交叉构建工具 | Debian bookworm，通过阿里云镜像获取 | 各 Debian 软件包的独立许可 |
| QEMU | 宿主安装的 QEMU | 主要为 GPL v2；参见安装来源附带的许可 |
| 可选 Mail 字体替换 | 已校验的官方音乐播放器原包中的 MiSans-Normal.ttf | 沿用原包的 MiSans 字体许可，工具同时保存 MiSans-LICENSE.txt；原版 Mail 的 Ark Pixel 字体及 SIL OFL 声明保留在原包中 |

`build/repository/` 保持预装应用的原始 tar 包、核心文件、索引和签名。其他商店应用按需下载到 `.cache/channel/objects/`，原始包及附带许可材料完整保留。`build/binaries.json` 提供原始二进制来源及哈希，`build/licenses/upstream/` 保留预装应用包和历史发行包附带的许可证与说明，`build/licenses/ffmpeg/` 保留 FFmpeg 许可。Linux、BusyBox、curl、FFmpeg 对应的完整源码保留在下载缓存和构建卷中；如果分发预构建镜像，需要同时履行其源码提供和通知义务。FFmpeg 源码 SHA-256 固定在下载脚本中，与 [Buildroot 2026.08 的 FFmpeg 哈希](https://github.com/buildroot/buildroot/blob/2026.08/package/ffmpeg/ffmpeg.hash) 对照。

项目自有代码使用 GPL-3.0-or-later，完整文本见 LICENSE。`guest/driver/c1sim.c` 单独采用 GPL-2.0-only，并含 SPDX 标识。第三方代码、字体、图片和发行程序的许可不因本项目打包而改变。
