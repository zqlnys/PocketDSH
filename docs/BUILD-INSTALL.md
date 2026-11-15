# 构建与安装

## 当前可用命令

```bash
cd /root/dsh/PocketDSH

# Linux：Swift 语法、YAML/JSON、守卫、SwiftPM 测试
make check

# 只跑 core + Lua 测试
make test

# 重新生成 AppIcon（需要 Pillow）
make icon
```

## 纯 Linux（xtool）

前置：

1. Swift 6.4+（本机 6.4.2 已安装）；
2. `xtool`（本机 `/tmp/xtool`，建议安装到 `/usr/local/bin/xtool`）；
3. 从 Apple 官方下载的 `Xcode.xip`；
4. `xtool setup` 登录 Apple 账号并构建 Darwin Swift SDK。

当前阻塞：

```text
xtool sdk status  -> Not installed
xtool auth status -> Logged out
Xcode.xip         -> absent
```

没有 SDK 时不能生成 iOS IPA。不会从非官方源下载 Apple SDK。

## 云端 macOS（备用验证路径）

仓库包含 `.github/workflows/ios.yml`：

```bash
gh workflow run "iOS CI" --ref main
gh run watch
gh run download <run-id> -n pocketdsh-ios-<run-number> -D ./download
```

已验证 run `43719958908` 的 artifact 包含：

```text
PocketDSH-unsigned.ipa
pocketdsh-home.png
pocketdsh-home-dark.png
pocketdsh-zh.png
pocketdsh-plugins.png
logs/
```

IPA SHA-256：

```text
874e421be96c6ae0a05ab68d1764fe481410de5cafcdcc5c096fec962f4b48bb
```

这条路径使用 GitHub macOS Runner，**不是**纯 Linux 构建。

## 签名安装（用户负责）

未签名 IPA 不能直接安装。最终签名方式：

- 当前已知条件：用户有签名证书；
- 可用 Sideloadly / 自己的签名流水线；
- 免费 Apple ID 通常 7 天有效期，具体限制以 Apple 为准；
- CI 不持有证书、私钥或 Apple ID。

## 状态

- 本地领域/Lua 测试：通过（16 个 SwiftPM 测试）
- XcodeGen 生成：通过
- iOS 编译：通过（GitHub macOS Runner，Xcode 26.3）
- iOS 测试：6/6 通过（含插件生命周期）
- 未签名 arm64 IPA：已生成
- 纯 Linux 构建：未完成（缺 Xcode.xip）
- IPA 签名：未执行
- 真机安装：未执行
