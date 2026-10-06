# Music Player 0.3.6 发布记录

2026-09-26 已发布正式应用仓库，并从公网重新下载完成回验。

- ID：`music-player`；显示名：`Music Player`；作者：`fwz233`
- 版本：`0.3.6`；入口：`music-player`；保留既有终端启动方式，清单为 `C1PKG-PACKAGE 1`
- 发布及回验时目录序列：179
- 服务端包大小：21,105,325 字节
- 服务端包 SHA256：`68d57a150ac4cee8a7729169d7615e427f50e1f044ff19ce10a01bace9b07956`
- 本地/服务端 ELF SHA256：`efba02839127d89e28f573c3391a76e7ba99697ea8c1ef64446a1d98654be836`
- 对应源码包 SHA256：`c78c47c40a4321509c8d0225de9752398796dde58399243b7546029b2c950a2e`

音量和视效设置默认保存到 `/storage/c1/music-player/{volume,display}.json`。歌曲仍在 `/storage/mtp/Music`，封面缓存仍在曲目旁。按维护者要求，不读取、迁移或删除旧 `/usr/data/c1/music-player` 数据，不在大分区不可用时回退至小分区。新目录没有状态时音量为 50，视效为静态。

最终发行目录：`../../build/music-player-0.3.6/publish-20260926-official-pdf-reviewed/`。只从最终对应源码包解出的应用、共享模块与包内锁定依赖，在全新缓存、离线环境通过主机 test/vet 和 Linux/MIPS hardfloat vet/build，重建 ELF 与发布程序逐字节相同。播放器 61 项测试通过、1 项可选用户封面样本测试跳过，共享 c1device 22 项通过。目标模拟器执行过版本及存储相关定向测试；未执行本版完整 MIPS 运行套件，也未完成真机按键、电子纸、音频或功耗验收。

本包原样提供 MiSans Normal 4.003 运行时字体，保留小米官方四页中英双语许可 PDF、全文提取文本与来源声明。字体依独立专有协议随完整 App 提供，不是 Apache/GPL 字体，不改编、不单独分发；源码包不重复收录 TTF，重建完整应用目录时使用同一包中的字体。GPL 对应源码以及 Go 模块原始源码和完整 BSD 许可均随包提供，故包体比旧版增大。许可核验见 `licenses/MiSans-SOURCE.md` 和 `THIRD_PARTY_NOTICES.md`。

通过固定 SSH 主机指纹的加密隧道及已有 fwz233 作者令牌发布。公网回验验证目录 Ed25519 签名、包哈希/长度、清单、全部 18 个 payload 文件及权限；与更新前相比，仅本应用及 Book Reader 的包改变，原 13 个应用均保留。没有重复上传或覆盖旧版本，也未上传用户音乐、受限项目源码或凭据。

回验证据：`../../build/release-2.9.11/apps-public-verified.json`，公开包：`../../build/release-2.9.11/music-player-public.tar.gz`。此应用已独立发布，不表示 C1ancher 2.9.11 核心已发布。请在 APP 列表刷新并更新至 0.3.6；未自动安装到设备。后续修改须升新版本，不改变本次发行包。
