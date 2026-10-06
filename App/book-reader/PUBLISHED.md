# Book Reader 0.1.23 发布记录

2026-09-26 已发布正式应用仓库，并从公网重新下载完成回验。

- ID：`book-reader`；显示名：`Book Reader`；作者：`fwz233`
- 版本：`0.1.23`；入口：`book-reader`；模式：`direct`
- 发布后的目录序列：178；与 Music Player 更新一同回验时为 179
- 服务端包大小：17,759,511 字节
- 服务端包 SHA256：`72506c453809656a05832762190240a1ac2b35af01acc31a6c5e3d2ef24b3ef6`
- 本地/服务端 ELF SHA256：`59c88fefc938bfcaa5b80a8200e06e350279163b6bc8d7668a1fc98493c7a253`
- 对应源码包 SHA256：`e10f286fa6fc749978c1cfa8bfc119eec03386d927b0986cfd4e8d9e8ecf5ab6`

默认进度、书签、错误日志及派生文档缓存改为 `/storage/c1/book-reader`；书籍仍在 `/storage/mtp/Book`。按维护者要求，不读取、迁移或删除旧 `/usr/data/c1/book-reader` 数据，不在大分区不可用时回退至小分区。新目录没有状态时按首次使用处理，旧阅读进度和书签不会自动恢复。

最终发行目录：`../../build/book-reader-0.1.23/publish-20260926-license-reviewed/`。只从最终源码包解出的源码及包内锁定依赖，在全新缓存、离线环境通过主机 test/vet 和 Linux/MIPS hardfloat vet/build，重建 ELF 与发布程序逐字节相同。阅读器 143 项测试通过、5 项可选用户书籍样本测试跳过，共享 c1device 22 项通过。目标模拟器执行过版本与存储相关定向测试；未执行本版完整 MIPS 运行套件或真机验收。

通过固定 SSH 主机指纹的加密隧道及已有 fwz233 作者令牌发布，未把令牌交给公网明文 HTTP。随后从公开固定 IP 验证目录 Ed25519 签名、包哈希/长度、direct 清单、全部 16 个 payload 文件及权限；其他应用均保留。包内包含完整必要许可与可离线重建的对应源码，因此包体比旧版增大。首次本地发布预检因 Windows 默认字符编码失败，在启动 SSH 和上传前退出；修复 UTF-8 解析后仅执行一次实际发布，没有覆盖既有版本。

回验证据：`../../build/release-2.9.11/apps-public-verified.json`，公开包：`../../build/release-2.9.11/book-reader-public.tar.gz`。此应用已独立发布，不表示 C1ancher 2.9.11 核心已发布。请在 APP 列表刷新并更新至 0.1.23；未自动安装到设备。后续修改须升新版本，不改变本次发行包。

---

# Book Reader 0.1.22 发布记录

2026-09-11 已使用 Windows Publisher 1.1.0 发布正式仓库，并重新下载服务器签名包完成回验。

- ID：`book-reader`；显示名：`Book Reader`；作者：`fwz233`
- 版本：`0.1.22`；入口：`book-reader`；模式：`direct`
- 验证时签名目录序列：153
- 服务端包大小：3,703,038 字节
- 服务端包 SHA256：`08221b4714f0b88913a6d6287e28cff76df4461d1905f9cbde397488282b7b1e`
- 本地/服务端 ELF SHA256：`81e34999617d13b0e4b508e6f055bf4193f1e65d1fbb72d71caf8e33ef32573e`
- 对应源码压缩包 SHA256：`62750244307a921a8a9cdc99d79fb6ef5bd2b2659c1852eb5f590e291475056a`

本地设备程序：`../../build/book-reader-0.1.22/payload/book-reader`。
冻结发布目录：`../../build/book-reader-0.1.22/publish-20260911-112801/`。目录内 `server-verification.json` 和 `published.tar.gz` 分别为核验结果与服务器正式包。

本版修正音量加下移/下一页、音量减上移/上一页；增加章节当前页/总页数，保留全书百分比；目录与书签四方向提示独立排列，O 跳转旁边增加 P 书签/删除快捷提示。功能细节和限制见 README.md、CHANGELOG.md。

最终代码通过 Windows 主机完整测试/vet、共用字体模块测试/vet；QEMU 10.0.13 完整 MIPS 测试包含三本本地样本的只读读取、全书原生字体分页及新增章节页码/书签导航验证。冻结源码在独立临时目录通过测试/vet，重建 ELF 与发布程序逐字节相同。

沿用已有授权 HTTP 发布服务和作者令牌，发布清单为 direct。首次公开回验遇到域名 403 和官方 IP 连接超时；仅重试读取后，通过官方 IP 校验 Ed25519 目录签名、包哈希/长度、清单和全部 8 个 payload 文件。没有重复上传或覆盖版本。用户书籍、私有缓存、令牌及模拟器均未打包上传。

ADB 当前没有设备，未自动安装，真机按键、电子纸及长时间阅读尚待验收。在 APP 列表刷新并更新 Book Reader 至 0.1.22 即可，原书籍、进度、书签无需手动迁移。后续变化须升新版本，不修改此冻结包。

---

# Book Reader 0.1.21 发布记录

2026-09-11 已使用 Windows Publisher 1.1.0 上传正式仓库，并下载服务器签名包完成回验。

- ID：`book-reader`；显示名：`Book Reader`；作者：`fwz233`
- 版本：`0.1.21`；入口：`book-reader`；模式：`direct`
- 验证时签名目录序列：150
- 服务端包大小：3,689,377 字节
- 服务端包 SHA256：`663aa7b7e6023ed4d01f3ad052aaf4ce9d69cd3e8e2e3023ff7c8621097e0dba`
- 本地/服务端 ELF SHA256：`c691c8c9165b0809233b96df20d65929b691f16b2c5c256ecd054da532643604`
- 对应源码压缩包 SHA256：`66a1951cc1411caff393eaee2cb3c16d2a690de2b49990ee90af00b2a85895db`

本地设备程序：`../../build/book-reader-0.1.21/payload/book-reader`。
冻结发布目录：`../../build/book-reader-0.1.21/publish-20260911-015633/`。该目录中的 `server-verification.json` 为核验结果，`published.tar.gz` 为从服务器重新下载的正式包。

沿用用户已允许的官方 HTTP 发布服务和已有 fwz233 作者令牌；令牌未进入软件包。验证公开目录的 Ed25519 签名，再依照签名目录检查下载包的 SHA256、长度、direct 清单及全部 8 个 payload 文件。访问验证使用官方固定 IP 回退。

本版包含包管理器同源点阵字体、花开堪折 TXT 标题与异常字节偏移修复、EPUB 纯文字读取、UTF-16 支持以及进度兼容处理。书籍只在本机只读测试，没有上传任何用户书籍。包内仅程序、文档、完整 GPL/OFL 及第三方许可说明、对应源码。

阅读器与共用字体模块的主机测试/vet、三本书的全章读取和原生字体分页通过；同一 MIPS 测试程序在 QEMU 10.0.13 下完整通过，三本真实书籍的 MIPS 读取/分页/缓存与持久化测试通过。对应源码在独立临时目录通过测试及 vet，并重建出逐字节相同的 MIPS ELF。旧模拟器异常及功能边界见 VALIDATION.md。

ADB 未连接设备，尚未在物理电子纸上进行字体观感、按键及长时间阅读验收，未自动安装到设备。可在 APP 列表刷新并更新 Book Reader 至 0.1.21；原 Book 目录无需改动。后续代码变化必须另升版本，不修改本次冻结包。
