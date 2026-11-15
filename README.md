# PocketDSH

以 **iPhone 本身为工作区** 的原生 iOS 编程 Agent App。参考 DSH 的任务/工具循环，但文件和执行都留在手机上。

- 原生 SwiftUI，iOS 17+
- 多项目本地工作区 + 文件树 / 编辑器 / 搜索 / diff
- 真实 Agent 工具：`list_files`、`read_file`、`write_file`、`search_files`、`apply_patch`、`run_lua`
- 嵌入 Lua 5.4.7，无 JIT、无 shell、无子进程
- DeepSeek API 推理，API Key 存 iOS Keychain
- 覆盖前备份、恢复历史、任务日志、取消、对话持久化
- 可安装的本地 Lua 插件：安装、加载、配置、启停、卸载，工具自动接入 Agent
- 极简浅色 / 深色界面，支持跟随系统 / 简体中文 / English
- Agent 可选 `deepseek-flash` / `deepseek-v4-pro`，可调推理强度（low/high/max）和思考模式
- Linux VPS 只开发/检查/编译，不执行用户任务

## 当前状态（2026-11-15）

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| 源码与工作区 UI | 已完成 | SwiftUI 多项目 / 文件树 / 编辑器 / 搜索 / diff |
| 核心与 Lua 测试 | 通过 | 16 个 SwiftPM 测试 |
| iOS 编译 | **通过** | run [#43746967664](https://github.com/zqlnys/PocketDSH/actions/runs/43746967664)，Xcode 26.3（公开仓库免费 macOS runner） |
| iOS 模拟器测试 | **通过** | 8/8，含插件完整生命周期测试 |
| 未签名 arm64 IPA | **已生成** | 1,527,872 bytes，SHA-256 `598310e1…30ff` |
| 插件系统 | **已实现并测试** | 安装 / 加载 / 配置 / 启停 / 卸载 / 工具接入 |
| 系统动作 | **部分实现，未真机验证** | 真实提醒 + 电话/消息准备，必须用户确认 |
| JIT / 运行时状态 | **Tier C POC 已跑通，主 App 未集成** | mmap 探测 + 自动 Tier A；iSH Alpine 在模拟器内实际运行 |
| 首次启动引导 | **已实现** | API Key 弹窗，可跳过并继续本地工作区 |
| 模型 / 推理强度 | **已实现** | 当前 DeepSeek V4 模型名 + low/high/max，思考模式默认关闭 |
| 纯 Linux iOS 构建 | **受阻** | 需要 Apple Xcode.xip + xtool Darwin SDK；本机没有该文件 |
| IPA 已签名 | 未执行 | 由用户的签名证书完成 |
| 真机安装 | 未执行 | 需要实体 iPhone 和签名 |

> 当前 IPA 来自 GitHub Actions macOS Runner，不是纯 Linux 构建。没有把“Linux 源码检查”冒充成“iOS 编译”。详见 [阶段验证](docs/PHASE1-VALIDATION.md) 和 [构建记录](docs/BUILD.md)。

## 界面预览（CI 模拟器实拍）

| 首次启动 | 首页 Light | 首页 Dark | 简体中文 |
| --- | --- | --- | --- |
| <img src="docs/screenshots/onboarding.jpg" width="200" alt="PocketDSH API key onboarding"> | <img src="docs/screenshots/home-light.jpg" width="200" alt="PocketDSH home light"> | <img src="docs/screenshots/home-dark.jpg" width="200" alt="PocketDSH home dark"> | <img src="docs/screenshots/home-zh.jpg" width="200" alt="PocketDSH 中文界面"> |

插件管理：

<img src="docs/screenshots/plugins.jpg" width="220" alt="PocketDSH plugins">

语言可在 Settings → Language 中切换。JIT 状态在 Settings → JIT & Runtime 中查看。

工作区采用桌面式左侧栏：

<img src="docs/screenshots/workspace.jpg" width="220" alt="PocketDSH desktop-style workspace">

截图由 CI 在 iPhone 17 Pro Max 模拟器中实际安装并启动 App 后抓取；不代表实体 iPhone 测试。

## 快速开始（Linux）

```bash
cd /root/dsh/PocketDSH
make check
```

已验证的本地能力：

- `PocketDSHCore` 8 个测试
- `LuaRuntime` 8 个测试（含 3 个持久插件运行时测试）
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
│   └── Features/                # Projects, Files, Editor, Agent, Changes, Plugins, Settings
├── PocketDSHTests/
├── examples/workspace-inspector.dshplugin/
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
- 未验证：真机安装、IPA 签名、TestFlight。
- 纯 Linux 构建未完成；当前 IPA 来自 GitHub macOS Runner。
- Tier C（内嵌 Linux/Node）尚未集成；iSH 可行性已验证，JIT 开关只做能力探测和偏好，不能授予权限。

## 文档

- [架构与兼容性判断](docs/ARCHITECTURE.md)
- [阶段 1 验证记录](docs/PHASE1-VALIDATION.md)
- [最新构建与 IPA 证据](docs/BUILD.md)
- [DSH / dsh-Go 兼容范围](docs/DSH-COMPATIBILITY.md)
- [PocketDSH 插件 SDK](docs/PLUGIN-SDK.md)
- [闹钟 / 电话 / 消息的系统能力边界](docs/SYSTEM-ACTIONS.md)
- [Tier C 本地 Linux 可行性](docs/TIER-C.md)
- [构建与安装](docs/BUILD-INSTALL.md)

## 许可

Lua 5.4.7 以 MIT 许可证 vendored，见 `Packages/PocketDSHKit/Sources/CLua/LICENSE`。
