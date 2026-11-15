# PocketDSH 架构

## 核心原则

1. **iPhone 是工作区**：项目文件、备份、会话和运行结果都在 iPhone 沙盒内。VPS 只负责开发、静态检查和（可选的）云端编译，不执行用户的 Agent 任务。
2. **真实文件操作**：Agent 的每个文件工具都经过路径守卫并操作真实文件；工具结果、stdout、stderr 和退出状态来自实际执行。
3. **可恢复**：覆盖已有文件前创建独立备份，记录 diff；恢复操作本身也记录为一次变更。
4. **能力边界明确**：没有 shell、`fork/exec`、JIT、任意系统二进制。Lua 运行时不可用的库直接返回错误，不转发到服务器。
5. **不把插件兼容说成移植**：当前实现是 DSH 风格的本地 Agent 工具子集，不是 Cordis/Node 插件运行时。

## 分层

```text
PocketDSH (SwiftUI app)
├── Projects           项目创建 / 导入 / 外部目录授权
├── Workspace          文件树 / 编辑器 / 搜索 / diff
├── Agent              DeepSeek 工具调用循环、任务日志、取消
├── Changes            备份 / 恢复 / 变更历史
└── Settings           Keychain、模型、步骤上限、运行时说明

PocketDSHKit (SwiftPM)
├── PocketDSHCore (Foundation only)
│   ├── WorkspacePathGuard
│   ├── WorkspaceFileSystem
│   ├── UnifiedPatch + LineDiff
│   ├── JSONValue / Agent models
│   └── ChangeRecord / ProjectRecord
└── LuaRuntime
    ├── CLua: Lua 5.4.7 C sources + pocket_lua bridge
    └── LuaEngine: async wrapper, output capture, cancellation/timeout
```

### 为什么拆成 SwiftPM 包

- `PocketDSHCore` 和 `LuaRuntime` 可以在 Linux 上真实执行测试；
- iOS App 只承担 UI、Keychain、URLSession 和文件选择器；
- Lua C 运行时可以在 iOS 和 Linux 上编译同一套源码；
- CI 可以分别报告领域测试与 iOS 测试。

## 文件工作区

### 项目类型

- **Managed**：复制到 `Documents/PocketDSHProjects/<UUID>/workspace`，App 完全管理。
- **External**：通过系统文件夹选择器授权，保存 security-scoped bookmark；写操作在授权范围内进行。删除项目只解除引用，不删除外部目录。

### 路径守卫

`WorkspacePathGuard`：

- 拒绝绝对路径、`~`、NUL；
- 标准化后要求位于 root 下；
- 对已存在的路径和 root 解析 symlink，阻止 symlink 逃逸；
- 所有工具和编辑器写入都经过该守卫。

### 文件工具

| 工具 | 实际行为 |
| --- | --- |
| `list_files` | 递归枚举真实目录，限制深度，隐藏内部目录 |
| `read_file` | 读取 UTF-8 文本，按行返回，限制 2 MB |
| `write_file` | 真实写文件；已存在时必须 `overwrite=true`，先备份再原子写入 |
| `search_files` | 递归字面搜索，跳过二进制和内部目录，返回 path:line |
| `apply_patch` | 解析 unified diff，多文件 hunk 全部校验后写入；支持上下文/增删行 |
| `run_lua` | 在 App 进程内运行 Lua，返回真实 stdout/stderr/exit code |
| `plugin__*` | 已启用本地 Lua 插件的工具；通过受控宿主 API 读写工作区 |

插件框架（PocketDSH 自定义，非上游 DSH）：

- 安装：文件夹导入或内置示例，复制到 App Support/Plugins；
- 加载：每个启用插件一个持久 Lua 5.4 状态；
- 生命周期：`activate(config)`、`handle_tool(name, args)`；
- 权限：`workspace.read`、`workspace.write`、`workspace.search`；
- 管理：启用/停用、配置、日志、卸载；
- Agent：插件工具 schema 动态加入请求；执行回到对应 Lua 运行时；
- 日志：真实 `plugin.log` + 宿主错误，持久化到插件日志。

`apply_patch` 当前限制：

- 只更新已有文件，不支持 `/dev/null` 创建/删除或重命名；
- hunk 必须严格匹配；不做模糊 fuzz；
- 删除行以 `---`/`+++` 开头时存在解析歧义，工具会报错而不是猜测；
- 所有写入仍有备份。

## Agent 循环

```text
用户任务
  -> 系统提示 + 历史消息 + 工具 schema
  -> DeepSeek /chat/completions (stream=false)
  -> assistant tool_calls
  -> 顺序执行工具（真实文件/真实 Lua）
  -> tool result 追加到消息
  -> 再次请求模型
  -> 无 tool_calls 时结束
```

- 最大步骤由设置控制（3–20，默认 10）。
- 每个 assistant/tool 事件持久化到项目元数据目录的 `agent-conversation.json`。
- 取消会取消 URLSession 请求和工具任务；Lua 运行时通过 C 层 atomic 标志与 1,000 指令 hook 中断。
- 网络中断不会影响本地文件和手动编辑；Agent 报错，工作区继续可用。
- 不使用流式 token；当前进度按“模型步骤 / 工具事件”展示，避免伪造 token 级进度。

## 本地执行运行时

选择 **Lua 5.4.7**，不是 shell：

- 纯 C，无 JIT，适合 iOS；
- 可设置指令 hook 和 deadline，能真实中断死循环；
- 不打开 `io`、`os`、`package`、`debug`，并移除 `dofile`/`loadfile`；
- 只暴露 `print` 与安全的 base/string/table/math/utf8/coroutine；
- 不支持网络、子进程、任意原生库加载。

已验证（Linux）：

- 算术/表/字符串执行；
- 真实 stdout；
- 运行时错误返回 exit code 1 和 stderr；
- `os/io/package/debug/dofile/loadfile` 均为 nil；
- `while true do end` 在 0.15 秒 deadline 下被中断。

iOS 编译和模拟器执行尚需 CI 验证（见阶段状态）。

## 可选 Linux 后端

**未实现。** 事实边界：

- 未越狱 iOS App 不能 `fork/exec` 任意系统二进制；
- 不能假设 Docker、PROOT、硬件虚拟化或 JIT 可用；
- iSH 一类项目是 x86 用户态 Linux ABI 仿真，不是完整系统仿真；集成它需要独立的内核/系统调用模拟层、rootfs、进程模型和大量适配，且仍有 App Store/JIT 限制；
- 完整 Linux 系统仿真需要虚拟化/JIT 或特殊 entitlement，未越狱设备不可假定；
- 当前 App 不会把命令发送到 VPS，也不把“终端界面”伪装成本地 Linux。

## DSH 运行时与插件兼容

审计对象：服务器上的 DSH `0.2.2-alpha.3`。

DSH 实际形态：

- Node 应用启动器 + profile；
- 插件基于 `@deepseek-ai/cordis`，通过配置树按顺序加载；
- 工具体系是 `@deepseek-ai/dsh-tools` 的 `ToolsService`，工具注册包含 schema、execution、output、presentation、取消 signal；
- 文件工具名称是 `read` / `write` / `edit` / `apply_patch` / `glob` / `grep`；
- 大量插件依赖 Node 原生模块、child_process、ripgrep 二进制、MCP、tmux/PowerShell 等。

PocketDSH 的兼容范围：

| 能力 | 状态 |
| --- | --- |
| 自定义模型工具调用循环 | 已实现 |
| 真实本地文件工具 | 已实现（命名和 schema 为 PocketDSH 自己的） |
| DSH 工具语义子集 | 部分对齐（read/write/search/patch） |
| PocketDSH 自定义 Lua 插件安装/加载/配置/启停/卸载 | **已实现** |
| 插件工具接入 Agent 循环 | **已实现** |
| 插件通过受控 API 操作 iPhone 工作区 | **已实现** |
| Cordis 插件加载 | 未实现 |
| Node 原生扩展插件 | 不兼容 |
| Bash / PowerShell / tmux 工具 | 不兼容 |
| MCP 客户端 | 未实现 |
| DSH session 日志格式导入/导出 | 未实现 |
| DSH profile / bundle 配置 | 不兼容 |

因此，**不能把本 App 宣称为完整 DSH 移植**。它是“DSH 使用体验启发的原生 iPhone Agent”。

### 对 dsh-Go 的审计结论

`ccch1mneyyy/dsh-Go` 是 Android Flutter 客户端 + 手机内 DSH 引擎：

- 插件是 npm/DSH/Cordis 包，逻辑跑在 Node 引擎；
- App 不执行插件代码；宿主桥接把插件声明的原生 UI/工具卡数据发给 Flutter；
- 其插件协议是 `dshGo` 清单、slots、JSON 视图和 bridge；
- 原生 App 不加载任意 Dart 或 Node 代码。

PocketDSH 的 iOS 约束不同：没有 Node/Cordis，也不应把主机下发的代码直接放进 App。因此当前实现是 **自定义 Lua 插件 + 受控宿主 API**，不是 dsh-Go 原生插槽协议，也不是上游 DSH 插件格式。完整说明见 [PLUGIN-SDK.md](PLUGIN-SDK.md)。

## 构建路径与限制

### 纯 Linux 构建

已实测：

- Linux Swift 6.4.2 对 `arm64-apple-ios17.0` 报错：
  `unable to load standard library for target 'arm64-apple-ios17.0'`；
- clang 可以生成 arm64 Mach-O `.o`，但没有 iOS SDK/SwiftUI/UIKit/链接所需的 Apple 二进制；
- 系统没有 `iPhoneOS*.sdk`、`Xcode*.xip`、`xcodebuild`；
- 已安装 `xtool 1.24.0`，但 `xtool sdk status` 为 Not installed、`xtool auth status` 为 Logged out。

官方可行的 Linux 路径是 **xtool + Apple 的 Xcode.xip 提取 Darwin Swift SDK**。这仍然需要 Apple 账号下载 Xcode.xip，且 Xcode 本身不运行。当前没有该文件，因此：

- **纯 Linux 构建未完成，不能声称已生成 IPA。**
- 本仓库同时保留 XcodeGen + GitHub Actions macOS 路径，用于实际编译验证；它是云端 macOS，不是“Linux 原生编译”。

### 签名

CI 只生成未签名 IPA。最终签名由用户自己的证书/Apple ID 完成。
