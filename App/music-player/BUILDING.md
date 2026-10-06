# 从对应源码重建 Music Player 0.3.6

源码归档包含 `App/music-player`、`App/c1device`、构建/打包脚本、完整 GPL/BSD/MiSans 协议，以及三个 Go 扩展依赖的完整原始源码 ZIP（`third-party/goproxy`）。原样 MiSans Normal 4.003 仅随使用它的应用 payload 分发；TTF 不是 ELF 编译输入，源码包不重复放入字体，避免单独字体分发。原字体 SHA256 为 `1a5f4112daaa9473747c6834041646cc9b2c338cb40ab5dbb2f0161f8968ca10`。

使用官方 Go 1.26.4（https://go.dev/dl/，源码 https://go.dev/dl/go1.26.4.src.tar.gz）。解出 source 后，在 Windows PowerShell 中令 `$source` 指向其绝对路径：

    $env:GOTOOLCHAIN = 'local'
    $env:GOWORK = 'off'
    $env:GOFLAGS = ''
    $env:GOPROXY = ([Uri]::new((Join-Path $source 'third-party/goproxy/'))).AbsoluteUri
    $env:GOSUMDB = 'off'
    $env:GOOS = 'windows'; $env:GOARCH = 'amd64'; $env:CGO_ENABLED = '0'
    cd "$source/App/c1device"
    go test -count=1 ./...
    go vet ./...
    cd "$source/App/music-player"
    go test -count=1 ./...
    go vet ./...
    $env:GOOS = 'linux'; $env:GOARCH = 'mipsle'; $env:GOMIPS = 'hardfloat'
    go build -trimpath -ldflags '-s -w -X main.version=0.3.6' -o music-player .

完整测试默认使用测试依赖内的 Go 字体，不需要下载 MiSans；可设 `C1_FONT_PATH` 为同一应用 payload 中的原字体再验证该字体。`GOSUMDB=off` 禁止联网查和数据库，`go.sum` 仍检查固定模块。Go 编译器/标准运行时作为一般工具链不重复收录，固定版本官方来源见上。所有模块 ZIP 保留其原始源码、许可证及各文件版权声明。

重建完整设备目录时使用 `build.ps1 -Version 0.3.6 -FontPath <同一应用payload/assets/MiSans-Normal.ttf> -OutputPath <新目录/music-player>`；须保留其配套完整许可。需要 WSL file/readelf 做脚本 ABI 检查。不要替换、修改或单独再分发 MiSans；完整协议和官方来源见 `licenses/MiSans-LICENSE.pdf`（官方四页原件）、`licenses/MiSans-LICENSE.txt`（完整提取文本）、`licenses/MiSans-SOURCE.md`。

打包：`py tools/prepare_release.py --version 0.3.6 --binary <已验证ELF> --font <原字体> --stage <不存在的新目录>`。工具没有字体的本机路径默认值，校验固定原始字体哈希，直接从允许清单生成新目录，绝不复制或覆盖旧 stage，不包含用户媒体、服务器/发布器/安装器/InkWars 或任何秘密。仅本地准备，不签名、不上传。
