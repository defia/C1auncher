# MiSans 许可来源和分发范围

- 官方完整协议 PDF：https://hyperos.mi.com/font-download/MiSans%E5%AD%97%E4%BD%93%E7%9F%A5%E8%AF%86%E4%BA%A7%E6%9D%83%E8%AE%B8%E5%8F%AF%E5%8D%8F%E8%AE%AE.pdf
- 取得日期：2026-09-26。原始 PDF 为四页中英双语、七节，79,535 bytes，SHA256：`4a93a27cd2bd81b3b5ecfd0a853144a876fa26938a93a68443c67d74172fcb86`。
- `MiSans-LICENSE.pdf` 保留官方原始字节；`MiSans-LICENSE.txt` 为全部四页提取文本，保留全部条款和双语内容，换行及部分汉字的 Unicode 表示来自 PDF。排版或文字提取有疑问时以原始 PDF 为准。
- https://hyperos.mi.com/font/download 仅有简短条件，不能替代完整 PDF。该网页 HTML SHA256 为 `ad53f0c0e8eb41d64a38985dd047014741331f9a61af0d0149cdb0d34dee1b0b`。
- 字体为 MiSans Normal 4.003，原始版权字段：Copyright © 2020-2023 Beijing Xiaomi Mobile Software Co.,Ltd. All Rights Reserved.
- 原始字体 SHA256：`1a5f4112daaa9473747c6834041646cc9b2c338cb40ab5dbb2f0161f8968ca10`。

Music Player 使用 MiSans 字体。依据协议第 2 节第 3 项的应用作品例外，原样字体仅随使用该字体的完整应用包提供；保留字体名称、全部字形、元数据及原始字节，不改编、不制作子集、不单独出售或再分发字体。按第 2 节第 1、4 项保留明确署名、版权声明及完整协议；亦须遵守该协议其他条款。字体不属于本项目 GPL 源码，也不得因与 GPL 程序同包就宣称为 GPL/Apache/OFL 字体。

对应源码归档不重复收录字体，避免变成独立字体分发。该 TTF 是独立运行时资源而非 Go ELF 编译输入；源码可独立测试/编译。需要复现完整应用包时，将同一应用 payload 的 `assets/MiSans-Normal.ttf` 通过 `build.ps1 -FontPath` / `prepare_release.py --font` 显式传入。完整协议 PDF、全部提取文本和本说明同时保留在源码与应用包中。

第三方字体工具仓库的 Apache 2.0 仅适用于其脚本，不是小米字体授权依据。旧 `publish-20260926-220319`（错误 Apache）和 `publish-20260926-license-reviewed`（仅网页短文本）已标记 REJECTED，不能发布。采用完整官方 PDF 的新目录为 `publish-20260926-official-pdf-reviewed`。

本说明是本次分发依据和限制记录，不是额外授权，也不替代官方协议。接收方使用、分发字体仍须遵守官方协议。
