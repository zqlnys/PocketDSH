# 阶段 1 验证记录

日期：2026-11-15

## 1. 结论

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 原生 SwiftUI 源码 | 已写出 | `PocketDSH/` |
| 模块化领域层 | 已完成 | `Packages/PocketDSHKit/Sources/PocketDSHCore` |
| 本地 Lua 运行时 | 已在 Linux 真实执行 | 5/5 测试 |
| 文件/补丁/备份逻辑 | 已在 Linux 真实执行 | 8/8 测试 |
| Lua 插件运行时 | 已在 Linux 真实执行 | 3/3 测试（加载、工具、宿主写文件） |
| 插件安装/配置/启停/卸载 | 已实现 | 源码与 UI |
| XcodeGen 工程生成 | 已在 Linux 验证 | XcodeGen 2.46.0 生成 `PocketDSH.xcodeproj` |
| 纯 Linux SwiftUI 编译 | **失败/受阻** | 无 Darwin Swift SDK / iOS SDK |
| xtool 纯 Linux 路径 | **未完成** | 无 Xcode.xip、未登录 Apple、SDK 未安装 |
| iOS 编译 | **已通过（macOS CI）** | run [#43725221625](https://github.com/zqlnys/PocketDSH/actions/runs/43725221625) |
| iOS 模拟器测试 | **已通过** | 6/6，含插件生命周期 |
| 可签名 arm64 IPA | **已生成** | 1.3 MB，SHA-256 `0ade9946…03d4` |
| 真机安装/运行 | 未执行 | 无设备连接 |

> 不能把“源码写完”或“Linux 测试通过”说成“iOS 编译成功”。本次 iOS 编译由 GitHub macOS Runner 完成，不是纯 Linux 构建。

## 2. Linux 工具链实测

### Swift

```text
Swift version 6.4.2 (swift-6.4.2-RELEASE)
Target: x86_64-unknown-linux-gnu

$ swiftc -target arm64-apple-ios17.0 -typecheck Splash.swift
error: unable to load standard library for target 'arm64-apple-ios17.0'
```

Linux 官方 Swift 工具链只包含 Linux 标准库，没有 Darwin/iOS Swift 标准库。

### Clang

```text
$ clang -target arm64-apple-ios17.0 -c probe.c -o probe.o
$ file probe.o
Mach-O 64-bit arm64 object
```

LLVM 可以生成 Mach-O **目标文件**，但这不等于可运行的 iOS App：

- 没有 SwiftUI/UIKit/Foundation 的 iOS SDK；
- 没有 iOS Swift 标准库；
- 没有链接所需的 Apple SDK stub/TBD；
- 没有签名、Info.plist 处理、资源编译（actool）等 Xcode 流程。

### xtool

```text
xtool 1.24.0
xtool auth status  -> Logged out
xtool sdk status   -> Not installed
Xcode.xip          -> absent
iPhoneOS*.sdk      -> absent
xcodebuild         -> absent
```

xtool 官方 Linux 安装文档要求先下载 Apple 的 `Xcode.xip`，再生成 Darwin Swift SDK。没有该文件，纯 Linux 路径无法继续。

不会从第三方镜像下载 Apple SDK，也不会把 Xcode 安装到 Linux 后声称已经构建。

## 3. 已完成的真实验证

### SwiftPM

```bash
cd Packages/PocketDSHKit
swift test
```

结果：

```text
PocketDSHCoreTests:        8 passed
LuaRuntimeTests:           5 passed
LuaPluginRuntimeTests:     3 passed
```

Lua 测试实际检查了：

- 算术、表、字符串、`print`；
- 运行时错误 exit code 1 + stderr；
- `os/io/package/debug/dofile/loadfile` 不存在；
- 死循环在 deadline 后被中断（0.15 秒测试）；
- 输出捕获。

### XcodeGen

```bash
xcodegen generate --spec project.yml
# Created project at .../PocketDSH.xcodeproj
```

生成的工程包含：

- app target `PocketDSH`
- test target `PocketDSHTests`
- 本地包引用 `Packages/PocketDSHKit`
- 产品 `PocketDSHCore`、`LuaRuntime`
- `TEST_HOST`、scheme、Info.plist

`PocketDSH.xcodeproj` 和 `Info.plist` 都是生成物，不入库。

### Swift 静态解析

```text
swiftc -parse: 35 Swift files OK
```

这仅是语法级 parse，不做 SwiftUI 类型检查。

## 4. 运行时可行性

选择 Lua 5.4.7，原因：

- 纯 C，无 JIT；
- iOS 可编译；
- 可通过 `lua_sethook` 实现指令级 deadline/取消；
- 不需要 `fork/exec`、动态库加载或系统 shell；
- 可以复用 Linux 测试验证语义。

不打开 `io`、`os`、`package`、`debug`，并移除 `dofile`/`loadfile`。因此 Lua 不能直接访问文件系统；文件访问由 Agent 工具完成。

## 5. 可选 Linux 后端

未实现。详细边界见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 6. 构建路径现状

- 已使用 GitHub Actions macOS Runner 完成 Debug/Release 编译、6 个 iOS 测试、截图和未签名 IPA；
- 纯 Linux 路径仍受 Xcode.xip / Darwin Swift SDK 阻塞；
- 用户可用自己的签名证书对 IPA 签名并在实体 iPhone 上安装；
- 真机安装和签名仍未验证。

如果需要纯 Linux 构建，必须提供合法 Apple `Xcode.xip` 并完成 `xtool setup`。当前不会从第三方镜像下载 SDK。
