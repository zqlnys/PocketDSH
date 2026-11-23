# PocketDSH 插件 SDK（自定义）

> **重要：这是 PocketDSH 自定义的 Lua 插件格式，不是上游 DSH 的 npm/Cordis 插件格式，也不是 dsh-Go 的原生插槽协议。**
>
> 当前 App 不嵌入 Node，不加载任意 `.node` 原生扩展，也不执行 Cordis/React 插件代码。插件在 iPhone 上的受控 Lua 运行时中执行，通过宿主 API 操作当前工作区。

## 1. 插件包结构

```text
my-plugin.dshplugin/
├── plugin.json       # 清单：id、权限、工具 schema、配置字段
├── main.lua          # 生命周期和工具实现
└── README.md         # 可选
```

安装方式：

- App 内 **Settings › Plugins › Install example**；或
- **Settings › Plugins › Install plugin from folder** 选择 `.dshplugin` 文件夹。

安装会复制到 App 的 Application Support 目录。卸载会删除插件目录和日志，不删除插件已经写入工作区的文件。

## 2. plugin.json

```json
{
  "id": "com.example.workspace-inspector",
  "name": "Workspace Inspector",
  "version": "1.0.0",
  "description": "Reads and writes real workspace files.",
  "author": "PocketDSH",
  "apiVersion": 1,
  "entry": "main.lua",
  "permissions": ["workspace.read", "workspace.write"],
  "tools": [
    {
      "name": "workspace_report",
      "description": "List real files.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": { "type": "string" }
        },
        "required": []
      }
    }
  ],
  "config": [
    {
      "key": "prefix",
      "title": "Note prefix",
      "type": "string",
      "defaultValue": "Pocket note"
    }
  ]
}
```

字段：

- `id`：3–81 字符，`[A-Za-z0-9._-]`，全局唯一。
- `apiVersion`：当前只支持 `1`。
- `entry`：相对路径，不允许 `..` 或绝对路径。
- `permissions`：支持 `workspace.read`、`workspace.write`、`workspace.search`。
- `tools`：至少一个；工具名会映射为模型可用的 `plugin__<safe-id>__<tool>`。
- `config`：`string` / `boolean` / `number`，在插件详情页编辑并持久化。

## 3. main.lua 生命周期

```lua
local config = {}

function activate(values)
  config = values or {}
  plugin.log("info", "activated")
end

function handle_tool(name, args)
  if name == "workspace_report" then
    local listing = plugin.workspace_list(args.path or ".", 2)
    return plugin.result("Files:\n" .. (listing or "unavailable"), false)
  end
  return plugin.result("Unknown tool: " .. name, true)
end
```

- `activate(config)`：可选；配置保存/启用/重载时调用。
- `handle_tool(name, args)`：必需；返回 `plugin.result(content, is_error)`，也可返回字符串或 `{content=..., is_error=...}`。
- 每次工具调用有 5 秒 deadline；超时/取消会中断 Lua 执行并返回错误。
- 插件状态在同一个运行时内持久；App 重启或重载后重新执行 `main.lua`。

## 4. 受控宿主 API

全局 `plugin` 表：

| API | 权限 | 行为 |
| --- | --- | --- |
| `plugin.log(level, message)` | — | 写入真实插件日志；level: debug/info/warning/error |
| `plugin.result(content, is_error)` | — | 构造工具结果 JSON |
| `plugin.workspace_list(path, depth)` | `workspace.read` | 返回真实文件列表 JSON |
| `plugin.workspace_read(path)` | `workspace.read` | 返回真实 UTF-8 文件内容 |
| `plugin.workspace_search(query, include)` | `workspace.read` | 返回 `{matches, truncated, scanned}`：匹配列表、是否因上限截断、读了多少文件 |
| `plugin.workspace_write(path, content, overwrite)` | `workspace.write` | 真实写入；覆盖会产生备份/diff |
| `plugin.workspace_root()` | `workspace.read` | 当前工作区绝对路径（仅用于显示/诊断） |
| `plugin.config()` | — | 当前配置 JSON |

所有路径都经过 `WorkspacePathGuard`：

- 拒绝绝对路径、`..`、symlink 逃逸；
- 只允许当前项目 root 或用户授权的外部目录；
- 写入仍有备份和变更记录。

## 5. 运行时沙箱

插件 Lua 状态：

- Lua 5.4.7，纯 C，无 JIT；
- 不打开 `io`、`os`、`package`、`debug`；
- 移除 `dofile`、`loadfile`；
- 无 shell、`fork/exec`、网络、动态库加载；
- 只能通过 `plugin.workspace_*` 宿主 API 访问工作区；
- 5 秒工具 deadline；插件连续报错会在 UI 显示日志和 `lastError`。

## 6. 管理能力

- 安装：文件夹选择器 / 内置示例；
- 加载：启用时创建 Lua 运行时并执行入口；
- 配置：在插件详情页编辑，保存后重载；
- 启停：开关即时更新 Store，并对当前工作区重载运行时；
- 卸载：删除插件目录、状态和日志；工作区文件保留；
- 日志：真实 `plugin.log` 和宿主错误都持久化到插件日志；
- Agent：启用插件的工具 schema 会加入下一次 DeepSeek 请求；工具调用回到对应 Lua 运行时执行。

## 7. 示例插件

仓库：

```text
examples/workspace-inspector.dshplugin/
```

工具：

- `workspace_report`：读取真实文件树；
- `write_pocket_note`：写入 `POCKET_NOTE.md`（可配置 prefix），经过路径守卫和备份系统。

安装后，在 Agent 中可以直接要求：

```text
List the real workspace, then write POCKET_NOTE.md saying the plugin ran on this iPhone.
```

模型会看到 `plugin__com_example_pocketdsh_workspace_inspector__workspace_report` 等工具。

## 8. 与上游 DSH 插件的关系

不兼容：

- npm 包 / `package.json` + `cordis.patch.yml` + `lib/index.js`；
- Cordis 服务注入、slots、React 客户端；
- Node 原生扩展、child_process、ripgrep 二进制、tmux/PowerShell。

我们复用了 DSH/dsh-Go 的产品概念：清单、工具、配置、启停、日志、受控工作区、权限声明。没有把自定义 Lua 插件称为“兼容全部 DSH 插件”。

未来若要在 iOS 运行上游插件，需要嵌入 Node/Cordis 运行时或实现兼容层；这需要单独的可行性验证，不能靠当前 Lua 系统冒充。
