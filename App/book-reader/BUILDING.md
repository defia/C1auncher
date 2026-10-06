# 从对应源码重建 Book Reader 0.1.23

源码归档包含 `App/book-reader`、`App/c1device`、位图输入 `assets/pkg-font.bin`、生成脚本及 `C1ancher/src/pkg/font_generated.h`、完整 GPL/OFL/BSD 许可，以及全部三个 Go 扩展依赖的原始源码 ZIP（位于 `third-party/goproxy`）。不包含核心实现、服务器、发布器、Windows 安装器、InkWars、用户书籍、凭据或缓存。

使用官方 Go 1.26.4（https://go.dev/dl/，工具链源码 https://go.dev/dl/go1.26.4.src.tar.gz）。从归档中解出 `source/` 后，在 Windows PowerShell 设置以下环境，`$source` 必须是解出的 source 目录的绝对路径：

    $env:GOTOOLCHAIN = 'local'
    $env:GOWORK = 'off'
    $env:GOFLAGS = ''
    $env:GOPROXY = ([Uri]::new((Join-Path $source 'third-party/goproxy/'))).AbsoluteUri
    $env:GOSUMDB = 'off'
    $env:GOOS = 'windows'; $env:GOARCH = 'amd64'; $env:CGO_ENABLED = '0'
    cd "$source/App/c1device"
    go test -count=1 ./...
    go vet ./...
    cd "$source/App/book-reader"
    go test -count=1 ./...
    go vet ./...
    $env:GOOS = 'linux'; $env:GOARCH = 'mipsle'; $env:GOMIPS = 'hardfloat'
    go build -trimpath -ldflags '-s -w -X main.version=0.1.23' -o book-reader .

`GOSUMDB=off` 只避免联网查询；`go.sum` 仍验证归档中固定模块。完整依赖源码、根许可证和各文件原有版权声明随模块 ZIP 保留。Go 编译器/标准运行时是一般工具链，不重复收录其源码包，使用上方固定版本官方来源。

也可运行 `build.ps1 -Version 0.1.23 -OutputPath <新输出路径>`（需要 WSL 的 file/readelf 检查 ABI）。验证 ELF 应与同次发布 payload 的 `book-reader` 逐字节一致；测试不使用用户书籍，可选样本测试未配置时明确跳过。复制/发布时保持应用许可证和源码归档完整。

打包工具 `tools/prepare_release.py --version 0.1.23 --binary <已验证ELF> --stage <不存在的新目录>` 仅冻结允许清单本地材料，不签名、不上传，且不修改旧目录。准备时必须能读取固定版本 Go 依赖缓存，或允许 Go 正常获取公共依赖。
