# PocketDSH

以 **iPhone 本身为工作区** 的原生 iOS 编程 Agent App。参考 DSH 的任务/工具循环，但文件和执行都留在手机上。

- 原生 SwiftUI，iOS 17+
- 多项目本地工作区 + 文件树 / 编辑器 / 搜索 / diff
- 真实 Agent 工具：`list_files`、`read_file`、`write_file`、`search_files`、`apply_patch`、`run_lua`
- 嵌入 Lua 5.4.7，无 JIT、无 shell、无子进程
- DeepSeek API 推理，API Key 存 iOS Keychain
- 覆盖前备份、恢复历史、任务日志、取消、对话持久化
- 可安装的本地 Lua 插件：安装、加载、配置、启停、卸载，工具自动接入 Agent
- Linux VPS 只开发/检查/编译，不执行用户任务

> 当前状态：源码、领域逻辑、Lua 运行时和 Linux 测试已完成；iOS 编译和可签名 IPA 尚需构建验证。纯 Linux iOS 构建需要 Apple 的 Xcode.xip 为 xtool 提供 SDK，本机没有该文件。详见 [阶段验证](docs/PHASE1-VALIDATION.md)。

## 快速开始（Linux）

```bash
cd /root/dsh/PocketDSH
make check
```

已验证的本地能力：

- `PocketDSHCore` 8 个测试
- `LuaRuntime` 5 个测试
- `swiftc -parse` 全部 Swift 源文件
- XcodeGen spec 解析和工程生成（Linux 上使用 XcodeGen 二进制）
- 无 Token / 私钥 / WebView / Flutter 依赖

> Linux 只能做语法级 Swift 检查和 SwiftPM 测试，不能编译 SwiftUI/iOS。

## 目录

```text
PocketDSH/
├── project.yml                  # XcodeGen 单一工程源
├── Packages/PocketDSHKit/
│   ├── Sources/PocketDSHCore/   # 路径守卫、文件系统、patch、diff、模型
│   ├── Sources/CLua/            # vendored Lua 5.4.7 + C bridge
│   ├── Sources/LuaRuntime/      # Swift async runtime
│   └── Tests/
├── PocketDSH/
│   ├── App/                     # AppState, root, navigation
│   ├── Core/Services/           # Keychain, DeepSeek, project store, agent loop
│   ├── Core/Design/             # theme / cards
│   └── Features/                # Projects, Files, Editor, Agent, Changes, Settings
├── PocketDSHTests/
├── scripts/
├── docs/
└── .github/workflows/ios.yml
```

## 构建路径

### 方案 A：纯 Linux（需要 Xcode.xip）

[xtool](https://github.com/xtool-org/xtool) 可以在 Linux 上交叉编译 SwiftUI iOS App，但它需要：

1. Swift 6.4 工具链（已装 6.4.2）；
2. 从 Apple 官方下载的 `Xcode.xip`；
3. `xtool setup` 登录 Apple 账号并构建 Darwin Swift SDK；
4. 可选 `usbmuxd` 用于真机安装。

这台 VPS 目前是：

```text
xtool 1.24.0          installed
xtool auth status     Logged out
xtool sdk status      Not installed
Xcode.xip             absent
```

因此 **纯 Linux IPA 尚未生成**。不会从非官方镜像下载 Apple SDK，也不会伪造构建产物。

### 方案 B：GitHub Actions macOS（当前用于编译验证）

仓库提供 `.github/workflows/ios.yml`：

- 选择最新稳定 Xcode
- XcodeGen 生成工程
- `swift test` 跑领域和 Lua 测试
- 编译 iOS 真机架构 Debug/Release
- 模拟器测试
- 启动 App 并抓取浅色/深色截图
- 生成未签名 `PocketDSH-unsigned.ipa`
- 上传日志、截图、IPA
- 最终 gate 不允许跳过测试

这是云端 macOS，不是 Linux 原生编译。是否使用它作为首个 IPA 构建路径，需要明确记录；不会把它称为“纯 Linux 构建”。

## 本地运行逻辑

```text
ProjectStore -> WorkspaceSession
  -> WorkspaceFileSystem (root + path guard + backups)
  -> ToolExecutor (real files, real Lua)
  -> AgentRunner (DeepSeek tool loop)
```

Agent 只会拿到相对路径。绝对路径、`..`、symlink 逃逸都会被 `WorkspacePathGuard` 拒绝。

## 安全与限制

- API Key：iOS Keychain，`AfterFirstUnlockThisDeviceOnly`；不写项目文件。
- 文件：仅在当前项目 root 或用户授权的 security-scoped 目录内。
- 网络：只访问 `https://api.deepseek.com`；本地文件/编辑器不依赖网络。
- 运行时：Lua 5.4，无 `io`/`os`/`package`/`debug`/`dofile`/`loadfile`，无 JIT、无 shell、无 `fork/exec`。
- 插件：**PocketDSH 自定义 Lua 插件格式**，不是上游 DSH Cordis 插件，也不是 dsh-Go 原生插槽协议。
- 未实现：Linux 用户态 ABI、完整系统仿真、Cordis/Node 原生插件、SSH/SFTP。
- 未验证：iOS 编译（等待 CI）、真机安装、签名、TestFlight。

## 文档

- [架构与兼容性判断](docs/ARCHITECTURE.md)
- [阶段 1 验证记录](docs/PHASE1-VALIDATION.md)
- [DSH / dsh-Go 兼容范围](docs/DSH-COMPATIBILITY.md)
- [PocketDSH 插件 SDK](docs/PLUGIN-SDK.md)
- [构建与安装](docs/BUILD-INSTALL.md)

## 许可

Lua 5.4.7 以 MIT 许可证 vendored，见 `Packages/PocketDSHKit/Sources/CLua/LICENSE`。
