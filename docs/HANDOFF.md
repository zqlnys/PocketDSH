# PocketDSH 交接文档

最后更新：2026-11-17
用途：换新会话后，先读这个文件，再继续开发。

---

## 0. 新会话第一件事

在工作区 `/root/dsh` 下执行：

```bash
cd /root/dsh/PocketDSH
git status -sb
git log --oneline -5
./tools/verify.sh          # 一条命令验证全部，先确认起点是绿的
cat docs/HANDOFF.md
cat docs/BUILD.md
cat docs/TIER-C.md
```

当前仓库：<https://github.com/zqlnys/PocketDSH>（**public**）
当前分支：`main`
当前源码头：见 `git log --oneline -1`
最新已验证构建 commit：`afe6128`（fix: Lab screenshots, and a plist that leaked into the shipped bundle）
最新构建 run：<https://github.com/zqlnys/PocketDSH/actions/runs/43818797679>（发布版）
Lab 构建 run：<https://github.com/zqlnys/PocketDSH/actions/runs/43818797674>
最新 artifact：`pocketdsh-ios-102`（发布版）/ `pocketdsh-lab-2`（Lab 版）
最新 IPA 本地路径：`/root/dsh/PocketDSH-builds/latest/PocketDSH-unsigned.ipa`

最新 IPA：

- 大小：5,295,073 bytes
- SHA-256：`237d0175e71c2fa058c835deae4fddb3ffc6cd91aa7b09f1b5ec8d141e370780`
- 未签名
- Xcode 26.3 / iPhone 17 Pro Max 模拟器 iOS 26.2

测试：

- SwiftPM：16/16
- iOS：8/8
- 没有真实 iPhone 安装、没有签名验证、没有 TestFlight。

> 注意：当前 IPA 来自 GitHub Actions macOS Runner，不是纯 Linux 构建。
> Linux 上只能做 Swift 语法检查、SwiftPM 测试、XcodeGen 生成，不能编译 SwiftUI/iOS。

---

## 1. 项目目标

以 **iPhone 本身为工作区** 的原生 iOS 编程 Agent App。

核心要求：

- 多项目本地文件
- 文件树 / 编辑器 / 搜索 / diff
- 真实工具：`list_files`、`read_file`、`write_file`、`search_files`、`apply_patch`
- Agent 对话/任务循环、取消、日志、持久化
- API Key 存 iOS Keychain
- 本地执行，不把 Files/工具转发到 VPS
- 可选 Linux 后端，但必须明确能力边界
- 插件必须真实可用：安装、加载、配置、启停、卸载、工具接入 Agent
- 不冒充上游 DSH/Cordis 插件兼容
- 极简 UI、中文、模型/推理控制
- JIT/Tier A/Tier C 状态与回退提示

参考项目：<https://github.com/ccch1mneyyy/dsh-Go>
（Android Flutter + 手机内 Node/DSH 引擎；PocketDSH 没有复刻它的完整插件运行时。）

---

## 1.5 两个构建目标（重要）

- **PocketDSH**（发布版）：不编译、不链接任何 iSH 代码，桥接头只暴露原创的执行探针。**无 GPL 义务，可以上架。** Tier C 编译成 stub。
- **PocketDSH-Lab**（开发版）：链接 iSH，完整 Tier C。**GPLv3 衍生作品，绝不发布。**

验证方法（发布版二进制必须全部 absent）：`asbestos`、`fakefs`、`realfs`、`pocket_shell`、`pocket_ish`、`4.20.69`、`libish`。实测通过。

CI 也分开了：`ios.yml` 构建发布版，`tierc-lab.yml` 构建 Lab 版并跑 Tier C 截图。

详见 [BUILD-TARGETS.md](BUILD-TARGETS.md)。

## 2. 当前真实可用能力

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 原生 SwiftUI iOS | 已实现 | iOS 17+ |
| 多项目本地工作区 | 已实现 | managed copy / 外部授权文件夹 |
| 文件树 / 编辑器 / 搜索 / diff | 已实现 | 文件内容真实读写 |
| `list_files` / `read_file` / `write_file` / `search_files` / `apply_patch` | 已实现 | 路径守卫、备份、变更记录 |
| `run_lua` | 已实现 | 内嵌 Lua 5.4.7，无 JIT/无 shell/无子进程 |
| 覆盖前备份 / 恢复 / Changes | 已实现 | 元数据在工作区外 |
| Agent 工具循环 | 已实现 | DeepSeek OpenAI-compatible `/chat/completions` |
| 取消 / 错误 / 日志 / 对话持久化 | 已实现 | |
| 首次启动 API Key 引导 | 已实现 | 可跳过，继续本地工作区 |
| 中文 / English / 跟随系统 | 已实现 | Settings → Language |
| 极简 UI | 已实现 | 纸张/墨绿、桌面式左侧导航栏 |
| 模型 | 已实现 | `deepseek-flash` / `deepseek-v4-pro` |
| 推理强度 | 已实现 | `low / high / max` |
| Thinking | 已实现 | 默认关闭 |
| 自定义 Lua 插件 | 已实现并测试 | 安装、加载、配置、启停、卸载、工具接入 |
| 示例插件 | 已实现 | `examples/workspace-inspector.dshplugin` |
| 系统提醒 | 已实现 | EventKit 真实提醒事项 |
| 本地通知闹钟 | 已实现 | `schedule_alert`，UserNotifications，不是 Clock App |
| 电话准备 | 已实现 | `prepare_call`，用户确认，不静默拨号 |
| 消息准备 | 已实现 | `prepare_message`，用户点发送，不自动发送 |
| JIT 探测 | 已实现 | mmap 可执行映射探测，不执行动态代码 |
| Tier 偏好 | 已实现 | Automatic / Tier A / Tier C，Tier C 受限提示 |
| Tier C smoke test | 已实现 | 主 App 内 iSH Alpine，无 JIT，实际 `uname` |
| `run_linux` | **已接入持久 shell** | 复用同一会话，每条 ~0.4s；会话不可用时回退一次性桥接并说明 |
| 持久 Linux shell | **模拟器已验证（Lab 版）** | 6 条命令同一会话，每条 0.4s；发布版不含此能力 |
| 文件工作区挂载到 Linux | **模拟器已验证（Lab 版）** | realfs 直通 + 只读包装层；真机未做 |
| 真机 / 签名 / TestFlight | 未完成 | |
| 上游 DSH/Cordis 插件兼容 | 未实现 | 当前是自定义 Lua 插件格式 |

---

## 3. Tier C 当前实现（重要）

### 已跑通

主 App 内链接 iSH 静态库 + Alpine rootfs，Tier C smoke test 实际输出：

```text
Linux pocketdsh 4.20.69-ish SUPER AWESOME ... i686 Linux
```

- 没有 JIT
- 运行在 App 进程内
- iSH 是 x86 用户态解释器 + Linux syscall 翻译
- 不是完整虚拟机
- 不是远程主机

截图：`docs/screenshots/tierc.jpg`

### 当前限制

- `run_linux` 一次 App 启动只支持一个命令；
- 命令只看到 Alpine rootfs；
- 不能看到 iPhone 项目文件；
- 没有持久 shell；
- 默认运行时仍是 Tier A；
- Tier C 只是显式实验工具，不是默认自动路由；
- iSH 是 **GPLv3**，分发前必须处理许可证；
- 没有真机验证。

### 已尝试但失败

- 把项目复制到 `rootfs/data/workspace`：Linux 看不到，因为 iSH fakefs 使用 SQLite 元数据；
- 向 `meta.db` 的 `stats` / `paths` 插入 `/workspace`：Linux 本地测试可以，但 iOS App 内仍不可见；
- 持久 shell：pthread / pipe 原型未稳定，已回退到一次性 smoke；

**根因**：fakefs（`fs/fake.h` 标了 `ISH_INTERNAL`）把文件登记在 sqlite 里，裸拷进去的文件不在库中。挂载项目目录本来就不该用 fakefs。

### 当前方案：realfs 直通挂载（已验证：run 43793080884 / build 86）

改用 iSH 的 `realfs`（`fs/real.h`）直接挂宿主目录：

```c
do_mount(&realfs, host_workspace_path, "/workspace", "", 0);
```

- 宿主与 guest 共享同一批 inode，没有复制、没有数据库；
- 不需要 `iOSFS.m`——App 容器内的 managed copy 用绝对路径即可，外部授权目录靠 `WorkspaceSession` 的 security-scoped access；
- 挂载结果写进命令输出（`[workspace] mounted ... -> /workspace`），失败时给出 errno；
- 传参链：`WorkspaceSession.rootURL → ToolExecutor → LinuxRuntime.run(command:workspace:) → pocket_ish_run → pocket_mount_workspace()`。

详见 [WORKSPACE-MOUNT.md](WORKSPACE-MOUNT.md)。

**验收结果（2026-11-16）**：模拟器实测通过。

```
[workspace] mounted /Users/runner/.../workspace -> /workspace
i686
[host readback] .tierc-probe.txt = tierc-write-ok
[host readback] workspace mount verified: guest write reached the host directory.
```

guest 在 `/workspace` 写入的文件被宿主读回，证明挂载双向可用。证据：[tierc/workspace-mounted.txt](tierc/workspace-mounted.txt)。

仍有一个捕获限制：`ls -la /workspace` 的输出进不了 5 秒窗口（i686 下单条外部命令要 1–3 秒）。这是单命令桥接的时序问题，不是挂载问题；持久 shell 会取代固定窗口。

### 后续可行路线

1. ~~用 iSH 的 iOSFS 或正确 fakefs 元数据~~ → 已改为 realfs 直通；
2. 用持久 shell 的 pty/pipe 协议，支持多命令、超时、停止；
3. 只有 backend 初始化成功才把 Tier C 标为可用，否则自动回退 Tier A；
4. 处理 GPLv3 源码/分发合规；
5. 真机测试；
6. 评估 `/workspace` 默认只读（`MS_READONLY_`），因为 realfs 写入会绕过 `WorkspaceFileSystem` 的备份与路径守卫。

---

## 4. 关键构建数据

### 最新绿色构建

- Workflow：`.github/workflows/ios.yml`
- Runner：`macos-15`
- Xcode：26.3
- 模拟器：iPhone 17 Pro Max, iOS 26.2
- Run：43763977454
- 耗时：约 11m24s
- `ci-summary.md`：0 errors / 6 tooling warnings

### 本地归档

```text
/root/dsh/PocketDSH-builds/43763977454/
├── PocketDSH-unsigned.ipa
├── pocketdsh-onboarding.png
├── pocketdsh-home.png
├── pocketdsh-home-dark.png
├── pocketdsh-zh.png
├── pocketdsh-plugins.png
├── pocketdsh-workspace.png
├── pocketdsh-tierc.png
├── TestResults.xcresult
├── BUILD.md
└── SHA256SUMS.txt
```

`/root/dsh/PocketDSH-builds/latest` 指向最新 run。

### 构建命令

```bash
cd /root/dsh/PocketDSH
gh workflow run "iOS CI" --ref main
gh run list --workflow "iOS CI" --limit 1
gh run watch <run-id> --interval 20 --exit-status
gh run download <run-id> -D ./download
```

公开仓库的 macOS standard runner 免费；私有仓库之前因支付/额度失败，已改 public 解决。

### 本地检查

```bash
cd /root/dsh/PocketDSH
./scripts/local_check.sh
# 或
make check
```

XcodeGen 二进制：

```text
/tmp/xcodegen-src/.build/debug/xcodegen
```

生成工程：

```bash
/tmp/xcodegen-src/.build/debug/xcodegen generate --spec project.yml
```

---

## 5. 重要文件

### App

```text
PocketDSH/App/PocketDSHApp.swift
PocketDSH/App/AppState.swift
PocketDSH/App/RootView.swift
PocketDSH/Core/Design/PocketTheme.swift
PocketDSH/Core/Design/Components.swift
PocketDSH/Core/Services/AgentRunner.swift
PocketDSH/Core/Services/DeepSeekClient.swift
PocketDSH/Core/Services/ToolExecutor.swift
PocketDSH/Core/Services/ToolCatalog.swift
PocketDSH/Core/Services/SystemActionStore.swift
PocketDSH/Core/Services/LinuxRuntime.swift
PocketDSH/Core/Services/JITCapability.swift
PocketDSH/Core/Services/PluginRuntimeManager.swift
PocketDSH/Core/Services/PluginStore.swift
```

### UI

```text
PocketDSH/Features/Projects/ProjectsView.swift
PocketDSH/Features/Workspace/WorkspaceView.swift
PocketDSH/Features/Workspace/FilesView.swift
PocketDSH/Features/Workspace/EditorView.swift
PocketDSH/Features/Workspace/ChangesView.swift
PocketDSH/Features/Workspace/DiffView.swift
PocketDSH/Features/Agent/AgentView.swift
PocketDSH/Features/Settings/SettingsView.swift
PocketDSH/Features/Settings/OnboardingView.swift
PocketDSH/Features/Settings/TierCSmokeView.swift
PocketDSH/Features/Plugins/PluginsView.swift
PocketDSH/Features/Agent/SystemActionSheet.swift
```

### 包 / C / Lua

```text
Packages/PocketDSHKit/Sources/PocketDSHCore/
Packages/PocketDSHKit/Sources/LuaRuntime/
Packages/PocketDSHKit/Sources/CLua/
  pocket_lua.c
  pocket_plugin.c
  Lua 5.4.7 sources
TierCProbe/ISHBridge.c
TierCProbe/ISHBridge.h
Vendor/iSH/
  include/
  lib/iphoneos/
  lib/iphonesimulator/
  rootfs/
```

### 文档

```text
docs/ARCHITECTURE.md
docs/PLUGIN-SDK.md
docs/DSH-COMPATIBILITY.md
docs/SYSTEM-ACTIONS.md
docs/TIER-C.md
docs/WORKSPACE-MOUNT.md   ← Tier C 项目工作区挂载实现
docs/PERSISTENT-SHELL.md  ← Tier C 持久 shell 设计与验证
docs/BUILD-TARGETS.md     ← 发布版 / Lab 版的许可边界与验证方法
docs/EXECUTION-PROBE.md   ← 真机能否执行运行时加载代码（已定论：不能）
docs/E2-BACKEND.md        ← E2 重写后端：策略、进度、踩坑记录
docs/BUILD.md
docs/BUILD-INSTALL.md
docs/PHASE1-VALIDATION.md
docs/IOS-LOCAL-LINUX.md   ← iOS 本地 Linux 三方案调研笔记
docs/HANDOFF.md  ← 本文件
```

---

## 6. UI 设计状态

当前风格：

- 暖白纸张背景 / 墨色文字 / 深绿强调
- 大标题衬线字体
- 扁平列表、细分隔线、16/12 pt 圆角
- 无大渐变、无重阴影
- 工作区左侧固定导航栏：
  - Agent / Files / Changes / Plugins / Settings
  - 项目首字母头像
  - Tier 状态
- 首次启动 API Key 引导
- 中文 / English / 跟随系统
- Agent 顶部紧凑状态：Ready / Flash / High / IDLE

截图：

```text
docs/screenshots/onboarding.jpg
docs/screenshots/home-light.jpg
docs/screenshots/home-dark.jpg
docs/screenshots/home-zh.jpg
docs/screenshots/workspace.jpg
docs/screenshots/plugins.jpg
docs/screenshots/tierc.jpg
```

UI 不是照抄 DSH；是参考“桌面式工作区”结构重做的。

---

## 7. 模型 / 推理

`SettingsStore`：

- `model`: `deepseek-flash` / `deepseek-v4-pro`
- `reasoningEffort`: `low` / `high` / `max`
- `thinkingEnabled`: 默认 false
- `language`: system / chinese / english

DeepSeekClient 请求会发送：

```json
{
  "model": "deepseek-flash",
  "thinking": { "type": "disabled" },
  "reasoning_effort": "high"
}
```

注意：

- 模型名按当前 DeepSeek API 文档；
- 没有真实 API Key 做端到端请求验证（CI 不提供 Key）；
- 真实模型调用、工具调用、错误处理仍需用户用自己的 Key 验证。

---

## 8. 系统动作边界

已实现：

- `schedule_reminder`：EventKit 真实提醒事项；
- `schedule_alert`：UserNotifications 本地通知（不是 Clock App 闹钟）；
- `prepare_message`：`MFMessageComposeViewController` 预填，用户点发送；
- `prepare_call`：`tel:`，用户确认。

明确不做：

- 静默拨号；
- 读取/自动回复 Messages；
- 自动发短信/iMessage；
- Clock App 系统闹钟（未接 AlarmKit）；
- 用描述文件/漏洞绕过沙盒。

详见 `docs/SYSTEM-ACTIONS.md`。

---

## 9. 插件系统

当前是 **PocketDSH 自定义 Lua 插件格式**：

```text
plugin.json
main.lua
```

生命周期：

- 安装（内置示例 / 文件夹导入）
- 加载
- 配置
- 启用 / 停用
- 卸载
- 工具暴露为 `plugin__<safe-id>__<tool>`
- Agent 使用 `ToolCatalog.specs + pluginRuntime.toolSpecs`

不是上游 DSH/Cordis 插件兼容。
不是 Node/npm 插件运行时。

示例：

```text
examples/workspace-inspector.dshplugin/
```

插件工具通过受控 host API 访问工作区，有权限与超时限制。

---

## 10. 已知问题 / 不要再踩

### 构建

- 私有仓库的 GitHub Actions 曾因账单失败；现在 public 已解决。
- 公开仓库 CI 比私有慢（6–20 分钟），偶尔模拟器 test 步骤很慢。
- 不要用 `-derivedDataPath` 配 `-target` 的 xcodebuild；用 `SYMROOT`。
- macOS 没有 `timeout` 命令，不要写 `timeout ...`。
- iSH iOS 构建需要 Homebrew LLVM/LLD（VDSO）。
- iSH 锁定的 commit 见 `docs/tierc/ios-static-libs.txt`。

### Tier C

- `xX_main_Xx` 的 argc 不包含 NULL；persistent shell 曾因把 NULL 算进 argc 崩溃。
- iSH fakefs 不是普通目录；不要直接拷文件到 `data/` 就认为 Linux 能看到。
- `simctl spawn` 运行裸 CLI 会 Trace/BPT；必须作为 App target。
- shell 任务结束后可能仍保持运行；一次性桥接使用观察窗口。
- 真机未测试。

### 系统动作

- iOS 模拟器没有真实电话/短信 UI；
- Reminders/Notifications 权限没有在真机完整验证；
- 不允许静默发送/拨号。

### 模型

- 没有真实 Key 的端到端验证；
- `deepseek-reasoner` 旧名已不用；
- thinking 默认关闭，避免工具调用兼容问题。

### 仓库

- `docs/IOS-LOCAL-LINUX.md` 是研究笔记：三套 iOS 本地 Linux 的实测与架构讨论，含 `Vendor/iSH` 的代码勘察（asbestos 已在库中但 kernel 仍绑 32 位 x86）。
  结论可以用，但不要把里面的推断当成当前实现。
- 不要提交任何 API Key、签名证书、`.p8`、`.mobileprovision`、`.env`。
- iSH 是 GPLv3；分发前处理合规。

---

## 11. 当前未完成（按优先级）

1. ~~**Tier C 项目工作区**~~ → 模拟器已验证（run 43793080884）
   - realfs 直通挂载，双向读写；
   - 遗留：默认只读开关、真机验证、写入绕过 WorkspaceFileSystem 备份的问题。

2. ~~**持久 shell**~~ → 模拟器已验证（run 43796926038）
   - 单会话 `/bin/sh`，pipe + sentinel 定界；
   - 6 条命令连续执行，每条 0.1s，外部命令退出不再杀掉会话；
   - 遗留：接进 `run_linux` 工具、与一次性 runner 的互斥、真机。

3. ~~**Tier 自动路由**~~ → 已完成（run 43799762866）
   - `LinuxRuntime` 发布 `SessionState`（idle / starting / active / failed）；
   - `activeTier` 只在「用户选了 Tier C **且** 会话真的在跑」时报告 Tier C；
   - `tierCRequestedButUnavailable` 把回退暴露到 UI，不静默；
   - Tier C 不再用 JIT 探测推断（它本来就不需要 JIT）。

4. ~~**/workspace 只读能力**~~ → **已实现并运行时验证**（build 97），默认仍读写
   - iSH 的 realfs 只记录 `MS_READONLY_` 不执行，直接传标志是装饰性的；
   - 加了 `readonly_realfs` 包装层：写模式 open 与全部路径级写操作返回 `EROFS`；
   - 开关 `LinuxRuntime.workspaceReadOnly`，变化时自动重建会话；
   - **默认值待产品决定**：读写让 guest 构建工具可用但绕过工作区备份，只读更安全；
   - 验证结果：只读下写命令返回 `Read-only file system`，`ls` 正常，宿主确认无文件产生，会话在被拒后仍然存活（run 43808219268）。

4. **真实 Key 端到端**
   - 用用户 Key 验证 DeepSeek 工具调用；
   - 保存日志，不泄露 Key。

5. **/workspace 默认挂载模式**（需产品决定）
   - 能力已实现：只读包装层，写操作返回 EROFS；
   - 默认仍是读写。要定的是：是否允许 guest 写绕过 `WorkspaceFileSystem` 的备份与路径守卫；
   - 决定后改 `LinuxRuntime.workspaceReadOnly` 默认值，并补一次「只读下写被拒」的运行时测试。

6. **真机 / 签名**
   - 用用户证书签 IPA；
   - 真机验证 Reminders/Notifications/电话/短信；
   - GPLv3 决策。

7. **上游 DSH/Cordis 插件**
   - 目前完全没实现；
   - 如要做，先明确只支持纯 JS 子集还是远程 Host。

---

## 11.5 后端路线的实测结论（2026-11-16）

**iOS 真机不能执行运行时加载的代码。** 两条路径都堵：

| 路径 | 真机结果 |
| --- | --- |
| 文件映射 + `mprotect(PROT_EXEC)` | `EACCES` |
| 匿名页 + 拷贝代码 + 跳转 | 进程被内核终止，无信号 |

崩溃报告给出内核判词：`EXC_BAD_ACCESS` / `KERN_PROTECTION_FAILURE` / `termination: CODESIGNING, Invalid Page` / `SIGKILL`，崩溃地址正是 anonymous mmap 那一页。调用栈落在 `pocket_exec_probe → ExecutionProbe.run()`。

测试用的是**开了开发者模式的自签构建**（`developerMode: 1`，`codeSigningValidationCategory: 5`），比商店版宽松，连它都被内核处决。

**因此**：native userland 路线排除，重写后端必须自带 aarch64 解释器，工期 4–6 个月（不是 1–2 个月）。

详见 [EXECUTION-PROBE.md](EXECUTION-PROBE.md)。

## 11.6 E2 后端进度（长期项目）

目标：不复用 iSH 的 GPL 代码，自写 aarch64 解释器 + syscall 层，产出许可自主的 Linux 运行时。

**策略**：核心是平台无关 C11，本机 `make test` 秒级反馈；iOS 只做 fd 与 rootfs 的薄封装。这条策略已在 M1 抓到 4 个真 bug，其中 2 个是硬错误。

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| M1 | 解释器 + ELF 加载 + 基础 syscall，跑通 hello world 与递归 fib | ✅ |
| M2 | SIMD/FP + 动态链接 + 更多 syscall，跑通 musl ld.so 与静态 busybox | ✅ |
| M3 | busybox / sh 脚本 / 管道 / 重定向 / here-document / 作业控制 | ✅ |
| M4 | python | ❌ 未完成，CPython 在 `getpath` 阶段仍有差异 |

**这三个里程碑不是「跑通一次」，是有可重复的验证**：

| 层 | 数量 | 命令 |
| --- | --- | --- |
| 单指令测试 | 232 | `make test` |
| M2 扫描（不崩溃） | 163 | `tools/m2-sweep.sh` |
| M3 扫描（不崩溃） | 93 | `tools/m3-sweep.sh` |
| 差分对比（与参考实现逐字节一致） | 182 | `tools/diff-test.sh` |
| 静态检查（无 Swift 编译器时的替代） | 6 类 | `tools/check-swift.sh` |

一条命令跑完：`./tools/verify.sh`。

**`grep`、`sed`、`awk` 与参考实现逐字节一致**——这是当初的目标，已达成。

**已知未解决**：CPython 启动差异、`pow` 运算符（`^`/`**`）、命令替换在管道中丢输出（测试工具层）、
`wait4` 只等一个作业成员（已用量子规避）。

代码在 `e2/`，文档在 [E2-BACKEND.md](E2-BACKEND.md)。**E2 不阻塞上架**——发布版与它无关。

## 12. 下一步建议

如果新会话只做一件事：

> **真机验证**（需要用户签名证书，当前跳过）。模拟器上 Tier C 的三块拼图都已跑通：realfs 挂载、持久 shell、真实 backend 报告。剩下的差异只有真机内存压力和签名。

第二件事：

> 真实 Key 端到端 Agent 工具调用；再真机签名。

第三件事：

> `/workspace` 默认只读（`do_mount` 的 `MS_READONLY_`），写入走显式开关，避免绕过 `WorkspaceFileSystem` 的备份与路径守卫。

---

## 13. 给新会话的启动提示

复制以下内容到新会话：

```text
继续 PocketDSH。先读 /root/dsh/PocketDSH/docs/HANDOFF.md、docs/BUILD.md、docs/TIER-C.md、docs/WORKSPACE-MOUNT.md、docs/PERSISTENT-SHELL.md，检查 git status 和最新 CI。当前最新已验证构建是 run 43799762866 / pocketdsh-ios-91。Tier C 三块已跑通：realfs 项目挂载（双向）、持久 shell（多命令、每条 ~0.4s）、真实 backend 报告（不伪造）。未完成：真机验证、GPLv3 分发决策、上游 DSH/Cordis 插件兼容。不要伪造已完成能力。
```

---

## 14. 关键链接

- 仓库：<https://github.com/zqlnys/PocketDSH>
- 最新 CI：<https://github.com/zqlnys/PocketDSH/actions/runs/43763977454>
- 最新 IPA：`/root/dsh/PocketDSH-builds/latest/PocketDSH-unsigned.ipa`
- 本地构建归档：`/root/dsh/PocketDSH-builds/`
- Tier C 证据：`docs/TIER-C.md`、`docs/tierc/`
- 系统动作边界：`docs/SYSTEM-ACTIONS.md`
- 插件 SDK：`docs/PLUGIN-SDK.md`
- DSH 兼容性：`docs/DSH-COMPATIBILITY.md`

---

## 15. 一句话状态

**PocketDSH 是一个可构建、可运行的原生 iOS 本地 Agent App。Tier C 已经完整可用：项目目录通过 realfs 挂载在 `/workspace`（默认读写，只读能力已就绪待决定默认值），`run_linux` 复用同一个持久 shell（每条命令 ~0.4s），Tier 状态只在会话真的在跑时才报告 Tier C。未完成：只读默认值的产品决定、真机验证、GPLv3 分发决策、上游 DSH/Cordis 插件兼容。最新可用构建 `pocketdsh-ios-98`（run 43808219268）。**
