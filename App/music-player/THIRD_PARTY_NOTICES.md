# 第三方许可说明

## 本应用及共享模块

Music Player 与 c1device 的第一方源代码按 GPL-3.0-only 分发，完整条款见 `LICENSE`。对应源码归档包含两个模块、构建/打包脚本、锁定版本、三个 golang.org/x 模块的完整原始源码 ZIP 与许可，离线重建方法见 `BUILDING.md`。Go 1.26.4 编译器及其运行时是标准构建工具链，官方源码为 https://go.dev/dl/go1.26.4.src.tar.gz 。

## Go / golang.org/x

实际依赖版本：Go 1.26.4、golang.org/x/image v0.45.0、golang.org/x/sys v0.47.0、golang.org/x/text v0.41.0。逐一核对的上游根 LICENSE 内容相同，为完整 BSD 三条款许可；以下全文适用于以上组件。额外专利条款和模块原始文件在对应源码归档中保留。测试所用 Go 字体的独立许可保留于 x/image 原始源码 ZIP 的 `font/gofont` 源文件中，不是随应用运行的 MiSans。

Copyright 2009 The Go Authors.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * Neither the name of Google LLC nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## MiSans 字体（独立运行时资源）

本软件使用 MiSans Normal 4.003，版权归 Beijing Xiaomi Mobile Software Co., Ltd.。字体完整原文许可见 `MiSans-LICENSE.pdf`（官方四页中英双语原件）和 `MiSans-LICENSE.txt`（全部提取文本），官方来源与限制见 `MiSans-SOURCE.md`。许可为小米专有的《MiSans 字体知识产权许可协议》，不是 Apache 2.0、GPL 或 SIL OFL。

本应用依据协议第 2 节第 3 项的应用作品例外随应用提供原样 TTF；并按第 2 节第 1、4 项保留明确署名、版权声明及完整协议。字体未改编、未制作子集，SHA256 为 `1a5f4112daaa9473747c6834041646cc9b2c338cb40ab5dbb2f0161f8968ca10`。不得将字体单独再分发或售卖，不得把本项目 GPL 声明扩展到字体。

源码归档保留字体协议、来源、构建说明，但不重复放入 TTF。字体不是 Go 编译输入；如需重建完整安装目录，应显式使用同一应用包中的 `assets/MiSans-Normal.ttf`。既有错误的 DSRKafuU 网站工具 Apache 许可已弃用，不作为字体授权依据。

## 外部播放器

程序调用设备提供的外部音频播放工具；本包不包含 ffplay/FFmpeg 等外部二进制，不冒充提供这些独立组件的对应源码或许可。若未来打包这些组件，须另行满足其许可。
