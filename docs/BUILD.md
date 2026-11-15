# 构建记录

## 最新绿色构建

| 项目 | 值 |
| --- | --- |
| 日期 | 2026-11-15 |
| 仓库 | [zqlnys/PocketDSH](https://github.com/zqlnys/PocketDSH)（private） |
| Workflow | iOS CI |
| Run | [#43718395658](https://github.com/zqlnys/PocketDSH/actions/runs/43718395658) |
| Commit | `a18411d` |
| 结果 | **success** |
| 耗时 | 约 3 分 27 秒 |
| Runner | `macos-15` |
| Xcode | 26.3 (17C529) |
| 模拟器 | iPhone 17 Pro Max, iOS 26.2 (arm64) |
| 构建方式 | GitHub macOS Runner（**不是纯 Linux**） |

### 阶段结果

| 阶段 | 结果 | 证据 |
| --- | --- | --- |
| Xcode 自动选择 | PASS | 26.3 |
| XcodeGen 生成 | PASS | 工程创建成功 |
| PocketDSHCore 测试 | PASS | 8/8 |
| Lua runtime 测试 | PASS | 8/8（5 runtime + 3 plugin runtime） |
| Xcode 真机架构 Debug 编译 | PASS | `** BUILD SUCCEEDED **` |
| iOS 模拟器测试 | PASS | 6/6，`** TEST SUCCEEDED **` |
| 插件生命周期测试 | PASS | install → enable → invoke → configure → disable → uninstall |
| 模拟器启动 | PASS | 首页浅色 / 深色 / 插件页截图 |
| Release 真机架构编译 | PASS | `** BUILD SUCCEEDED **` |
| 未签名 IPA | PASS | `PocketDSH-unsigned.ipa` |

### 插件验证

iOS 测试 `PluginLifecycleTests.testInstallEnableInvokeConfigureDisableUninstall` 实际执行：

1. 把内置示例插件写入 App 的插件目录；
2. 创建持久 Lua 运行时并加载 `main.lua`；
3. 把插件工具 `write_pocket_note` 以 `plugin__...` 名称接入工具表；
4. 调用工具，通过受控 host API 写入真实工作区文件 `PLUGIN_TEST.md`；
5. 校验文件内容与插件日志；
6. 修改配置并重载；
7. 禁用插件，确认工具表为空；
8. 卸载，确认插件目录删除，工作区文件保留。

测试 workspace 是 iOS 模拟器中的临时真实目录，不是 mock 文件系统。

### IPA

| 项目 | 值 |
| --- | --- |
| Artifact | `pocketdsh-ios-7/PocketDSH-unsigned.ipa` |
| Bundle ID | `com.example.pocketdsh` |
| Version | 0.1.0 (1) |
| MinimumOSVersion | 17.0 |
| UIDeviceFamily | `[1]`（iPhone） |
| 架构 | `arm64` Mach-O |
| 签名 | **未签名**；无 `_CodeSignature` / `mobileprovision` |
| 大小 | 1,351,150 bytes |
| SHA-256 | `17b434870de817557a7997ac7ca89a2f7a29c93fe55f35c23536a570f6ddf295` |

下载：

```bash
gh run download 43718395658 -n pocketdsh-ios-7 -D ./download
gh run download 43718395658 -n pocketdsh-test-results-7 -D ./test-results
```

### Warnings

5 条 warning 均为工具层提示，不是 Swift 编译 warning：

- `appintentsmetadataprocessor`: 没有 AppIntents 依赖，跳过元数据提取（4 次构建各一条）。
- `xcodebuild`: 同一模拟器有 arm64/x86_64 两个 destination，使用第一个匹配项。

没有编译错误、测试失败或伪造结果。

### 截图

| 首页 Light | 首页 Dark | 插件管理 |
| --- | --- | --- |
| ![Home light](screenshots/home-light.jpg) | ![Home dark](screenshots/home-dark.jpg) | ![Plugins](screenshots/plugins.jpg) |

## 修复记录

| 轮次 | 失败 | 修复 |
| --- | --- | --- |
| 1 | `withSecurityScope` 在 iOS 不可用 | 改为 iOS 的 `.minimalBookmark` / `.withoutUI` |
| 2 | 插件工具名测试在非隔离上下文调用 `@MainActor` | 测试标注 `@MainActor` |
| 3 | 一个未使用的 `try?` warning | 改为 `_ = try?` |
| 4 | 插件行重复 chevron | 行内改为状态 pill，详情页统一导航 |

## 纯 Linux 构建

仍未完成：

```text
xtool 1.24.0          installed
xtool auth status     Logged out
xtool sdk status      Not installed
Xcode.xip             absent
swiftc -target arm64-apple-ios17.0
  -> unable to load standard library
```

xtool 需要合法 Apple Xcode.xip 生成 Darwin Swift SDK。没有该文件时不能把当前 macOS CI 产物描述成“Linux 原生编译”。

## 尚未验证

| 阶段 | 状态 |
| --- | --- |
| IPA 已签名 | 未执行 |
| 真机安装 | 未执行 |
| 真实设备运行 | 未执行 |
| TestFlight | 未配置 |
| 纯 Linux iOS 构建 | 未完成（需要 Xcode.xip） |
| 完整 DSH/Node 插件兼容 | 未实现（自定义 Lua 插件已实现） |
