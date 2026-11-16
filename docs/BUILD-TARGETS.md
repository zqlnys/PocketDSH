# 构建目标与许可边界

状态：已实现并验证（run 43818797679 / 43818797674）。
最后更新：2026-11-16

---

## 一、两个 target，一个能发，一个不能

| | **PocketDSH**（发布） | **PocketDSH-Lab**（开发） |
| --- | --- | --- |
| Bundle ID | `com.example.pocketdsh` | `com.example.pocketdsh.lab` |
| 是否链接 iSH | **否** | 是 |
| 是否含 rootfs | **否** | 是 |
| 编译标志 | 无 | `TIER_C_AVAILABLE` |
| 桥接头 | `RuntimeOnly-Bridging-Header.h`（只暴露探针） | `TierCProbe-Bridging-Header.h` |
| Tier C | 编译成 stub，调用返回「此版本不含后端」 | 完整可用 |
| 许可 | **无 GPL 义务** | **GPLv3 衍生作品，绝不发布** |
| IPA 体积 | 1.5 MB | 5.3 MB |

## 二、为什么必须编译期排除

只把功能关掉不够。GPL 的义务绑定在**分发受版权保护的代码**上，而静态链接 `libish.a` 会把 iSH 的代码整段编进你的二进制。功能开关改变不了「你的 app 里含有 GPL 代码」这个事实。

所以排除发生在三个地方，缺一不可：

1. **源文件**：发布版不编译 `TierCProbe/ISHBridge.c`、`ISHBridge.c`、`ISHPersistentShell.c`
2. **链接**：发布版的 `OTHER_LDFLAGS` 不含 `-lish -lish_emu -lfakefs`
3. **头文件**：发布版用独立的桥接头，连 iSH 的头都引用不到

第 3 条容易被忽略，但它是结构性保证：即使有人后来误加了源文件，桥接头也暴露不出后端接口。

## 三、验证方法

发布版二进制必须通过符号检查：

```bash
unzip -q PocketDSH-unsigned.ipa
BIN=Payload/PocketDSH.app/PocketDSH
for pat in asbestos fakefs realfs pocket_shell pocket_ish 4.20.69 libish; do
    printf "%-14s " "$pat"
    strings -a "$BIN" | grep -q "$pat" && echo FOUND || echo absent
done
```

**实测结果（run 43818797679，build 102）：全部 absent。**

同时确认 app bundle 里没有 `rootfs/`，`PocketDSH.app` 只有预期的那几个文件。

## 四、CI 分工

| Workflow | 构建什么 | 触发条件 |
| --- | --- | --- |
| `ios.yml` | PocketDSH（发布） | 推送到 main（排除文档）、PR、手动 |
| `tierc-lab.yml` | PocketDSH-Lab（开发） | 改动后端文件、手动 |

Tier C 的截图（正常挂载 + 只读挂载）只在 Lab workflow 里跑，因为发布版没有这个能力。

主 CI 仍然保留执行探针的截图——那个探针是原创代码，两个 target 都包含。

## 五、改代码时的注意事项

- **发布版新增文件**：如果要在发布版里加 C 代码，必须确认它不是 GPL 来源，并加进 `project.yml` 的 PocketDSH target
- **共享 Swift 代码**：`LinuxRuntime`、`PersistentShellSession`、`TierCSmokeView` 都有 `#if TIER_C_AVAILABLE` 双分支，改一个要改另一个
- **不要给发布版加 `TIER_C_AVAILABLE`**，那会把 stub 换成真实现，链接立刻失败
- **Lab 版产物不要上传到任何公开分发渠道**

## 六、当前边界

- 发布版：Tier A（原生）+ Lua 插件 + 执行探针 + 系统动作。Tier C 明确不可用
- Lab 版：以上 + 完整 Tier C（iSH Alpine、持久 shell、`/workspace` 挂载、只读挂载）
- 真机：两个 target 都未验证（等用户签名）
