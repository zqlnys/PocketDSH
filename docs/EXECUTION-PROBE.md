# 执行探测：iOS 能不能跑运行时加载的代码

状态：**已定论（真机）**。2026-11-16
结论：**不能。E2 必须自带 aarch64 解释器。**

---

## 一、为什么测这个

E2（重写 syscall 层、脱离 GPL）有两种形态，工作量差 4 倍：

| 形态 | 前提 | 工期 |
| --- | --- | --- |
| **native userland** | 进程能执行自己加载的 aarch64 代码 | 1–2 月 |
| **解释器 + syscall 层** | 不能，必须软件解码每条指令 | 4–6 月 |

宿主是 aarch64，guest 也是 aarch64，如果能直接执行，就只需要转发 syscall（proot 模型）。这一点值得花半天验证，因为它值三个月。

## 二、三次实测

### 模拟器（不代表真机）

```
anonymous executable mapping          denied (EACCES)
map code file read-only               ok
mprotect(PROT_EXEC) on file mapping   granted
execute loaded code                   returned 42
```

### 真机，第一次（build 102，只测文件映射）

```
anonymous executable mapping          granted
map code file read-only               ok
mprotect(PROT_EXEC) on file mapping   denied (EACCES)
execute loaded code                   not attempted
```

**和模拟器完全相反。** iOS 拒绝把未签名文件的映射提升为可执行，但愿意分配匿名 RWX 页。

### 真机，第二次（build 103，补测匿名页执行）

```
anonymous executable mapping          granted
map code file read-only               ok
mprotect(PROT_EXEC) on file mapping   denied
execute code copied into anonymous page    →  进程直接终止，无信号
```

进入 probe 页面即闪退。设置的 `SIGSEGV` / `SIGBUS` / `SIGILL` 保护**一个都没触发**——进程在一瞬间消失。

#### 崩溃报告给出的判词

用户真机日志 `PocketDSH-2026-11-16-152126.ips`：

```json
"exception": {
  "type": "EXC_BAD_ACCESS",
  "signal": "SIGKILL",
  "subtype": "KERN_PROTECTION_FAILURE at 0x0000000124618000"
},
"termination": {
  "namespace": "CODESIGNING",
  "indicator": "Invalid Page",
  "code": 2
},
"vmRegionInfo": "VM_ALLOCATE  124618000-12461c000 [16K] rw-/rwx SM=PRV"
```

四件事同时成立：

1. **崩溃地址正好是 anonymous mmap 那一页**（`VM_ALLOCATE`，16K = 我们申请的 4 页）
2. **失败原因是 `KERN_PROTECTION_FAILURE`**——页保护不让取指
3. **终止原因归到 `CODESIGNING` / `Invalid Page`**——不是普通内存错误，是代码签名判定
4. **信号是 `SIGKILL`**——内核终止，`SIGSEGV` 这类 handler 永远不会被调用

调用栈精确落在崩点：

```
? +?                                     ← 匿名页中的代码
pocket_exec_probe +324                   ← RuntimeExecProbe.c
ExecutionProbe.run() +88                 ← ExecutionProbe.swift
closure #2 in ExecProbeView.body.getter  ← 页面打开时触发
```

**签名状态也同样记录在案**：`developerMode: 1`、`codeSigningValidationCategory: 5`（开发签名）、自签 Team `5846AK7HYT`。也就是说，**在一个开了开发者模式的开发签名构建上，这条路径照样被内核处决。**

## 三、结论

**iOS 对可执行页的强制发生在内核层，不是应用能拦截的错误路径。**

两条加载路径都不可用：

| 路径 | 表现 | 原因 |
| --- | --- | --- |
| 文件映射 + `mprotect(PROT_EXEC)` | `EACCES` | 文件不在 trust cache / 未签名 |
| 匿名页 + 拷贝代码 + 跳转 | 进程被终止，无信号 | 取指时内核判定页不可执行 |

**关键旁证**：测试用的是**自签构建**，带 `get-task-allow`，权限比 App Store 版本宽松。连它都被这样处理，说明限制来自代码签名与内存保护机制本身，不是某个 entitlement 能换来的。

**因此**：native userland 路线排除，E2 = 解释器 + syscall 层，工期 4–6 个月。

## 四、对架构的影响

```
guest ELF (aarch64)
  → [自写 aarch64 解释器逐条解码]     ← 新增，3–4 周核心 + 1–2 周浮点
  → [自写 syscall 层转发到 iOS libc]   ← 3–4 周
  → iOS
```

需要自己实现的部分：

1. **aarch64 解释器**：定长 32-bit 解码、31 个通用寄存器、load/store、分支、标志位、标量浮点
2. **syscall 层**：~120 个 POSIX 调用，`fork`/`clone` 用 pthread 模拟
3. **ELF 加载器**：解析、映射、重定位、TLS
4. **文件系统**：目录映射（realfs 思路）、`/proc` 与 `/dev` 最小实现

`kernel/` 和 `fs/` 不能复用 iSH 的代码——那正是 GPL 的来源。

## 五、一个被证伪的猜想

之前从 ServerBox 的 `/proc/self/maps` 推断它是「aarch64 解释执行」，理由是主程序的段全是 `r--p`、没有 `r-xp`。**这个推断现在得到独立验证**：不是它不想 native 执行，是 iOS 根本不给。

它也走的是解释器路线，5–10× 的减速来自 aarch64 比 x86 好解释，不是来自去掉了解释层。

## 六、如何复现

`TierCProbe/RuntimeExecProbe.c`，两个入口：

- `pocket_exec_probe(&result, 0)` — 安全子集（匿名映射、文件映射、mprotect、文件执行）。默认路径，不会崩。
- `pocket_exec_probe(&result, 1)` — 额外执行匿名页。**会终止进程**，只在明确需要复现时用。

App 内：Settings → Execution probe，默认跑安全子集；「Also run anonymous execution (may crash)」按钮才走危险路径。

## 七、剩下没验证的

- **真机内存压力**：Tier C 在真机上能撑多大 rootfs、能跑多重的东西，模拟器测不出来。
- **商店版策略是否更严**：虽然自签版已经不行，但商店版理论上只会更严，不会更松。这条对 E2 无影响（反正走解释器），对 Lab 版的实际可用性有影响。
