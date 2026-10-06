# C1-Slim QEMU 模拟器

在电脑上运行 C1-Slim 的 Linux MIPS 二进制。QEMU 提供 MIPS 系统，客体适配 `/dev/epaper_lcd`、evdev、电源 sysfs 和音频；浏览器显示程序输出的 296×152 电子纸画面并发送按键，声音由电脑播放。

默认运行官方签名发行程序，固定核心为 **2.9.12 / 序列 30**。也可以加载同仓库编译的核心或自己的设备应用，ELF 字节保持不变。版本、签名和来源见 `release.json`；第三方依赖见 [许可说明](THIRD_PARTY_NOTICES.md)。

## 首次构建和启动

宿主需要 Python 3.11+、QEMU、curl、支持 Ed25519 的 OpenSSL，以及已启动的 Docker 或 Podman。Linux 安装发行版的 `qemu-system-mips`、`qemu-utils` 等；Windows 安装 Python、QEMU、curl 和 OpenSSL 并加入 PATH，容器需要 Linux 环境。实际验证宿主为 Apple Silicon macOS；Windows/Linux 宿主尚未执行验证。

macOS 使用 Homebrew 安装，并在运行构建和更新脚本的终端选择 OpenSSL 3：

```sh
brew install qemu python podman openssl@3
export PATH="$(brew --prefix openssl@3)/bin:$PATH"
```

macOS 系统自带的 LibreSSL 不支持此验签命令需要的 `-rawin` 参数。

在仓库根目录执行：

```sh
cd simulator
python3 scripts/build.py
python3 start.py
```

可以指定 `python3 scripts/build.py --engine docker` 或 `--engine podman`。Podman 用户须先启动 Podman machine。构建下载固定版本源码和官方发行材料，编译 Linux 6.6.60、BusyBox、curl、FFmpeg 和设备适配代码，需要数 GB 空间；结果和下载缓存不进入 Git。构建容器使用阿里云 Debian 镜像。

打开 <http://127.0.0.1:8765>。日常启动只需 `python3 start.py`，不需要容器。Ctrl+C 正常退出，或在另一个终端执行 `python3 start.py --stop`。正常关闭先同步并卸载数据盘；用户应用、配置和媒体保存在 `runtime/data.qcow2`。

可以用 `--port 8766 --disk runtime/another.qcow2` 运行独立实例，并用 `--stop --port 8766` 关闭。镜像重新打包、更新或切换本地程序前应停止使用该镜像的实例。

## 调试同仓库核心

首次构建客体后，使用同一个构建容器运行核心主机测试和 MIPS 交叉编译：

```sh
python3 scripts/build_core.py
python3 start.py --core-dir ../C1ancher/build
```

Linux / WSL 也可在仓库根直接执行 `make -C C1ancher -j2 host-test all`，然后在 `simulator/` 使用上述启动命令。`--core-dir` 需要完整的四个静态 MIPS ELF：`C1ancher`、`C1ancher-launcher`、`c1pkg`、`c1updater`；程序重新编译后，停止并再次运行该命令即可加载。

本地核心标记为 `local`，不会被标记为官方签名版本。启动时先用官方包管理器安装预装应用，再在内存中挂载本地核心；官方更新仓库和数据盘内的核心文件保持官方版本。本地核心默认使用独立的 `runtime/dev-data.qcow2`，可以显式指定 `--disk`。这里验证桌面和包管理器的运行，未模拟设备的完整守护启动链、签名发布与核心升级事务。

核心主机测试会调用容器内的 curl，并从 `/dev/shm` 加载 Bash 终端模块。构建镜像包含 curl，`build_core.py` 给该独立容器的 `/dev/shm` 设置可执行 tmpfs；容器默认的 `noexec` 会导致终端空闲关闭测试失败。

导入程序会保留在生成的 initramfs 中。恢复官方运行环境：

```sh
python3 scripts/pack_guest.py
python3 start.py
```

## 运行自己的设备应用

在 `simulator/` 执行：

```sh
python3 start.py --binary /absolute/path/my-app --resources /absolute/path/assets
```

不需要素材时省略 `--resources`。浏览器应用列表增加「自己的 MIPS 程序」。程序以 `/opt/custom/app` 启动，工作目录为 `/opt/custom`，资源按原目录结构复制；资源符号链接须先展开。要求为静态 Linux ELF32 MIPS 小端，推荐 o32、MIPS32/MIPS32r2、硬浮点，导入时检查 ELF 与 SHA-256。

例如已构建的 Go 图形应用可通过该入口运行，并提供与发行包相同的字体等资源。使用纯终端示例 `examples/hello/build/c1-example` 时，应从模拟器里的原版 TERMINAL 运行程序来显示终端输出；「自己的 MIPS 程序」入口不会自动为纯终端应用分配电子纸终端。

## 屏幕、键盘和声音

- 方向键导航，Enter 确定，Esc 返回，Home 回首页；也可点击网页设备键盘，按住按键模拟长按。电源短按锁屏/解锁。
- 字母默认小写。终端中 Ctrl 对应设备 OK 修饰键；单按 Shift 切换原机大小写/符号层，电脑数字键与网页字符按钮发送设备组合键。
- 原版网易云音乐 0.2.9、flomo 0.1.2 使用「音量＋ + Q–P」输入数字。模拟器按实际读取 evdev 的应用自动切换，返回桌面后恢复终端规则。
- 音频使用 AC97 虚拟声卡和 ALSA/OSS。MIPS 客体内解码 MP3、FLAC、AAC，支持 HTTP/HTTPS、播放器请求头、软件音量和 Pinao PCM；宿主默认音频后端自动选择，也可使用 `--audio-backend none` 或 `--audio-record output.wav`。
- 商店点击安装时，宿主通过官方 HTTPS 按需下载原始包，按已验签索引校验大小及 SHA-256，再交给原版 MIPS `c1pkg` 验签安装。原包持久缓存，后续可离线重装；损坏缓存会重新下载。

客体 curl 仅支持 HTTP；应用商店通过本机 HTTP 桥接宿主 HTTPS。Go 应用保留自身网络实现，音频解码器支持客体 HTTPS 并校验证书和主机名。官方 Bad Apple 0.1.1 包是无声动画。

### Mail 0.1.0 中文方框

Mail 0.1.0 原包的 `ark-pixel-10px-zh_cn.ttf` 缺少“邮、表、选、择、步、退”等字形，会把这些字画成方框。这是原包字体缺字，模拟器接收到的画面已经含有方框。

安装 Mail 后，退出 Mail，在 `simulator/` 执行：

```sh
python3 scripts/fix_mail_font.py
```

重新打开 Mail 即可使用完整中文字体。工具从已校验的音乐播放器原包提取 MiSans 字体和许可证，只替换已安装 Mail 的字体资源，原始 ELF 和下载的官方包保持不变。原字体保存在同目录的 `.ttf.c1sim-original` 文件中，MiSans 声明保存在 `assets/c1sim-MiSans-LICENSE.txt`。恢复原字体：

```sh
python3 scripts/fix_mail_font.py --restore
```

这是模拟器中的可选兼容处理，显示样式会改用 MiSans；工具会检查 Mail 版本、原始 ELF 和字体哈希，拒绝覆盖未知版本或自行修改的字体。其他实例使用 `--url http://127.0.0.1:8766`。Mail 更新或重装后应重新检查原包是否已补齐字体。

如需为网易云导入自己的 Cookie，在启动模拟器后运行：

```sh
python3 scripts/import_netease_cookie.py /path/to/cookie.txt
```

文件是一行 Cookie 请求头的值，例如 `MUSIC_U=...; __csrf=...;`，不要包含 `Cookie:` 前缀。工具仅向本机模拟器传输，写入客体 `/usr/data/c1/netease-music/cookie.txt`，权限为 600；导入后重新打开应用。Cookie 是账号凭据，应放在仓库外，不提交到 Git。

## 操作和调试入口

```sh
python3 scripts/console.py 'uname -a; ps; c1pkg list'
python3 scripts/capture_frame.py build/frame.png
python3 start.py --gdb 1234
```

GDB 端口只监听本机，用于 QEMU 客体调试。串口操作和本机 API 都使用当前实例；指定独立端口时给脚本加 `--url http://127.0.0.1:8766`。本机管理接口监听 loopback，并验证随机会话 token。

## 更新官方发行程序

```sh
python3 scripts/fetch_channel.py --check
```

`--check` 只查询已验签的正式核心通道和商店索引。更新前停止实例，备份数据盘、`release.json`、`build/binaries.json` 和 `build/images/rootfs.cpio.gz`，然后运行：

```sh
python3 scripts/fetch_channel.py --update
python3 scripts/pack_guest.py
python3 start.py
```

脚本用官方安装包的公开信任公钥分别验证核心清单与应用索引，再核对文件大小和哈希。构建预装六个应用，其余按需下载。已构建工作区的发行程序更新无需编译 Linux。固定版本在服务端移除且缓存缺失时，需要显式查询并选择可用版本。

## 验证

在 `simulator/` 运行，下列脚本的结果均写到忽略的 `build/verification/`：

```sh
python3 scripts/repository_cache_test.py
python3 scripts/core_test.py --core-dir ../C1ancher/build
python3 scripts/audio_test.py
python3 scripts/online_audio_test.py
python3 scripts/online_repository_test.py
python3 scripts/netease_keyboard_test.py
python3 scripts/flomo_keyboard_test.py
python3 scripts/cookie_import_test.py
python3 scripts/mail_font_test.py
```

集成脚本使用独立 QEMU 和临时数据盘；端口默认 8766，不操作默认用户盘。核心测试检查本地 ELF 实际挂载、屏幕、按键、终端输入与重启恢复。音频测试需要宿主 `ffmpeg`、`openssl` 生成样本和测试证书，数字键测试需要 Node.js；日常运行不需要这些测试依赖。

对已启动实例还可执行 `python3 scripts/smoke_test.py`，检查七个程序的 framebuffer、按键、电源锁屏与客体 ELF 哈希。商店包下载检查为 `python3 scripts/repository_test.py badapple netease-music`；不指定应用会下载检查整个索引。

整合验证的基线、结果和限制见 [验证记录](docs/validation.md)。

## 模拟范围

这是应用兼容环境：QEMU Malta + 24Kf、256 MB RAM、296×152 一位电子纸接口、两个 evdev 输入设备、电源 sysfs、256 MB 持久磁盘、AC97 音频和用户网络。Malta 不等同于原机 Ingenic SoC；电子纸波形/刷新时序、无线硬件、USB 模式、休眠耗电、原厂固件及整机升级流程需实机验证。模拟器的运行结果不能代替硬件验收。

## 内核来源与原机内核

这里的内核是**自行编译的上游 Linux 6.6.60，不是自行编写的 Linux 内核，也不是从设备提取的原厂内核**。[下载脚本](scripts/fetch_sources.py) 获取并校验源码，[内核配置](guest/linux.config) 选择 MIPS Malta、MIPS32r2、evdev 和虚拟声卡等功能，[构建脚本](guest/build.sh) 用 MIPS 小端交叉工具链生成 `build/images/vmlinux`。

模拟器自有代码主要是 [设备兼容模块](guest/driver/c1sim.c)、[屏幕与控制桥](guest/bridge.c)、初始化脚本以及音频适配工具。兼容模块向原始应用提供电子纸、键盘和电源等设备接口；应用仍执行自己的 MIPS 指令，通过 Linux 系统调用运行。我们是在现成内核上补设备接口，没有重写内核的调度、内存管理或文件系统。

客体用户空间同样是为模拟器组装的：基础工具使用 BusyBox，核心终端会回退到 BusyBox 的 `sh`；未打包原机 Bash、中文输入法或完整厂商文件系统。Bash 专用的终端空闲验证模块在构建容器中有主机测试，但其原机运行行为不属于当前 QEMU 客体验证范围。

提取原机内核有助于核对内核版本、配置、驱动和启动要求，但**原机内核通常不能直接换进当前 Malta 环境可靠启动**。同为 MIPS 并不意味着主板一致：原机驱动会访问其 SoC 的寄存器、时钟、中断控制器、DMA、存储及显示/音频外设。QEMU 的 `-M malta` 提供的是另一块主板的硬件模型，详见 [QEMU MIPS 文档](https://www.qemu.org/docs/master/system/target-mips.html)。原机内核或设备树描述硬件，不会让 QEMU 自动实现这些硬件。

如果目标扩展为原厂启动链、驱动、休眠或固件行为的验证，更完整的方案是先取得原机内核、设备树/板级配置和文件系统，再实现或移植匹配的 QEMU 主板及外设模型，按原机启动约定加载。只拿到一个内核镜像还不够。当前目标是运行和调试 C1auncher 及普通设备应用，使用 Malta Linux 加设备接口兼容模块更容易维护；该层的成功运行不能证明原机硬件行为一致。

## 许可

自有模拟器代码为 GPL-3.0-or-later，完整文本见 [LICENSE](LICENSE)。Linux 模块 `guest/driver/c1sim.c` 为 GPL-2.0-only；发行程序及其他依赖遵循各自许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。不随 Git 分发镜像、第三方源码缓存、个人媒体或账号数据。
