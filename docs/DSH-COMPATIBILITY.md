# DSH 兼容性审计

审计基于服务器上的 DSH `0.2.2-alpha.3`。

## DSH 实际运行时

- `@deepseek-ai/dsh` 是 Node 启动器；
- profile 由多个 bundle/patch 层组成；
- 插件基于 `@deepseek-ai/cordis`，按配置树加载；
- 工具由 `@deepseek-ai/dsh-tools` 的 `ToolsService` 注册；
- 工具定义包含 schema、execute、output schema、presentation、取消 signal；
- 文件工具实际名称是 `read`、`write`、`edit`、`apply_patch`、`glob`、`grep`，不是 PocketDSH 的 `list_files` 等命名；
- 很多官方插件依赖 Node 原生模块、`child_process`、ripgrep 二进制、MCP、Bash/PowerShell、tmux 等。

## PocketDSH 已实现

| 能力 | 状态 |
| --- | --- |
| 模型 tool_calls 循环 | 是 |
| 工具 schema | 是，PocketDSH 自有命名 |
| 真实本地文件读取/写入/搜索/补丁 | 是 |
| 覆盖前备份 + diff | 是 |
| 取消任务 | 是（网络请求 / 文件工具 / Lua deadline） |
| 对话/事件持久化 | 是 |
| 本地 Lua 执行 | 是 |
| 项目级工作区 | 是 |
| Keychain | 是 |

## 明确不兼容

| DSH 能力 | 原因 | 状态 |
| --- | --- | --- |
| PocketDSH 自定义 Lua 插件（安装/加载/配置/启停/卸载） | 自有格式，运行在 App 内 | **已实现** |
| 插件工具接入 Agent | 动态 schema + 宿主执行 | **已实现** |
| 插件受控操作工作区 | workspace.read/write/search 权限 | **已实现** |
| Cordis 插件加载 | 需要 Node + Cordis 运行时 | 未实现 |
| Node 原生扩展 | iOS 不能加载任意 `.node` 原生模块 | 不兼容 |
| Bash / PowerShell / tmux | iOS 无 fork/exec 和系统 shell | 不兼容 |
| ripgrep 二进制工具 | 不能启动任意二进制；本地用 Swift 搜索 | 不兼容 |
| MCP 客户端 | 未实现；需要独立传输与权限模型 | 未实现 |
| DSH session log 格式 | 自有 JSON 格式 | 未实现 |
| DSH profile / bundle 配置 | 自有 SwiftUI/AppState | 不兼容 |
| 子 Agent / Agent Team | 未实现 | 未实现 |

## 工具名称映射

| PocketDSH | DSH 近似 | 语义差异 |
| --- | --- | --- |
| `list_files` | `glob` / 目录 view | 参数和输出格式不同 |
| `read_file` | `read` | 行号格式不同；2 MB 上限 |
| `write_file` | `write` | 已有文件必须显式 `overwrite`；自动备份 |
| `search_files` | `grep` | 字面搜索，不是 ripgrep 正则 |
| `apply_patch` | `apply_patch` | 只支持已有文件 unified diff；无 Codex 全功能 |
| `run_lua` | 无对应 | PocketDSH 自有安全运行时 |

## dsh-Go 的插件实现（参考）

`ccch1mneyyy/dsh-Go` 是 Android Flutter 客户端 + 手机内 DSH Node 引擎：

- 插件仍是 npm/DSH/Cordis 包，逻辑在 Node 引擎里；
- App 不执行插件代码；桥接层把插件声明的工具卡、面板、胶囊等 JSON 视图交给 Flutter 原生渲染；
- 其 `dshGo` 清单、slots、caps 是 dsh-Go 自己的协议，不是上游 DSH 核心插件协议；
- 插件安装/启停/配置/卸载由引擎的插件管理器完成。

iOS 版不能直接照搬：没有 Node/Cordis，也不应运行来自主机的任意代码。PocketDSH 采用了不同的自定义 Lua 插件系统，保留了产品语义（清单、工具、权限、配置、启停、日志），但**格式不兼容**。详见 [PLUGIN-SDK.md](PLUGIN-SDK.md)。

## 结论

PocketDSH 是 DSH 体验启发的原生 iPhone Agent，**不是上游 DSH 或 dsh-Go 的插件宿主**。自定义 tool calling 和自定义 Lua 插件不等于上游插件兼容。

未来可选的兼容层：

1. 导入/导出 DSH session JSON 的只读投影；
2. 用 WebSocket/HTTP 连接桌面 DSH 的 MCP/ACP（作为可选远程扩展，不替代本地工作区）；
3. 重新实现部分工具语义，但不加载 Cordis 插件。
