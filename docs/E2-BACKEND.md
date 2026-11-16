# E2 后端：许可自由的 Linux 运行时

状态：**M1 完成；M2 进行中**（musl 动态链接器完整运行；文件 syscall 层已接入，正在解决地址空间分配）
最后更新：2026-11-16

---

## 一、为什么要有它

`Vendor/iSH` 是 GPLv3，静态链接进 app 就传染整个二进制。发布版已经通过拆分 target 规避（见 [BUILD-TARGETS.md](BUILD-TARGETS.md)），但那样 Lab 版仍被限制，闭源商业化也办不到。

E2 是重写：**不复用 iSH 的任何代码**，自己实现 aarch64 解释器 + syscall 层。产出的代码许可自主，开源、闭源、双许可都可以。

### 为什么不走 native userland（更省事的那条）

实测结论见 [EXECUTION-PROBE.md](EXECUTION-PROBE.md)：iOS 内核拒绝执行运行时加载的代码。

- 文件映射 + `mprotect(PROT_EXEC)` → `EACCES`
- 匿名页 + 拷贝代码 + 跳转 → `SIGKILL`，termination namespace `CODESIGNING` / `Invalid Page`

自签构建（带 `get-task-allow`）也不例外。所以必须软件解码每条指令。

---

## 二、核心策略：平台无关 + 本地可测

iOS 上最大的成本不是写代码，是**每个改动要等 11–15 分钟 CI，且只能在模拟器验证**。

所以整个核心是纯 C11、零平台依赖：

```
e2/
├── include/e2/      cpu.h  memory.h  exec.h  loader.h  syscall.h
├── src/             memory.c  exec.c  syscall.c  loader.c
└── tests/           main.c + tests/guest/*.s（交叉编译的测试程序）
```

本机 `make test` 几秒钟出结果。iOS 层只做两件事：提供 fd 后端、提供 rootfs 目录。

**这条策略的价值已经被验证**：M1 期间抓到四个真 bug，其中两个是硬错误（越界写、SP 被破坏），如果每次都要等模拟器，代价是几个小时。

---

## 三、当前能力（M1）

| 组件 | 状态 |
| --- | --- |
| 指令解码 | MOVZ/MOVN/MOVK、ADR/ADRP、ADD/SUB（immediate + shifted register）、AND/ORR/EOR/ANDS（shifted register + **immediate**）、**位域 SBFM/BFM/UBFM**、B/BL、B.cond、CBZ/CBNZ、BR/BLR/RET、LDP/STP（整数 + **SIMD**）、LDR/STR（unsigned offset + **unscaled/pre/post** + **register offset**）、**DUP / SIMD 载入存储**、SVC、NOP |
| 向量 | 32 个 128-bit 寄存器，按 lane 访问；DUP 广播、SIMD 单/双寄存器载入存储 |
| 寄存器 | X0–X30 + SP 分离，NZCV 正确建模 |
| 内存 | 平坦 48 位地址空间，区域管理 + 权限位 + `mprotect` |
| ELF | ELF64 aarch64 静态与 **ET_DYN（PIE，固定基址 0x40000000）**，多段、页对齐、加载期可写后降权限；报告 load bias / entry / PT_DYNAMIC / phdr |
| 重定位 | **R_AARCH64_RELATIVE**、**GLOB_DAT / JUMP_SLOT**（经 DT_SYMTAB 自解析）；未定义符号明确拒绝 |
| syscall | `write`、`writev`、`exit`、`exit_group`；未知调用返回 `-ENOSYS` |
| 测试 | **47 项**，含四个交叉编译的真实 ELF（hello world、递归 fib、SIMD memset、freestanding C 程序）和一个合成的 PIE 重定位镜像 |

### 已验证的端到端用例

- `hello.elf`：`mov` + `adr` + `svc write` + `svc exit`，输出逐字节校验
- `fib.elf`：递归 fib(10)=55，含 STP/LDP 栈帧、BL/RET、`b.lt`、栈上 spill 槽，1956 条指令
- `memset.elf`：`dup v0.16b` 广播 + `stp q0,q0` 批量写 + `ldrb` 逐字节校验
- `cprog.elf`：clang 编译的 freestanding C 程序，结构体按值返回、字节加载、循环、除法强度削减；输出校验 `sum=42`、`idx=3568`
- 合成的 PIE 镜像：验证 bias 计算与 RELATIVE 重定位结果

### 正在进行：M2 的动态链接

用 Alpine 的真实 `ld-musl-aarch64.so.1`（static-pie，723 KB）验证：

| 阶段 | 结果 |
| --- | --- |
| 加载 | ✅ bias 0x40000000，entry 0x400696bc |
| 重定位 | ✅ RELATIVE + GLOB_DAT 全部应用 |
| 执行 | 459 条指令后失败，卡在自己对栈上 auxv 的处理 |

### M2 里程碑：musl 动态链接器完整运行

```
[guest fd2] musl libc (aarch64)
[guest fd2] Version 1.2.5
[guest fd2] Usage:  [options] [--] pathname [args]
[guest exit 1]
```

自举、libc 初始化、`main()`、参数处理、stdio、正常退出全部走通。这是 E2 第一次运行真实的 aarch64 动态链接器并让它做完自己的事。

接入真实 rootfs 后，它继续往下走：

```
set_tid_address
openat("/bin/busybox")  -> fd 3
read(fd, 960)           -> -EISDIR
writev(stderr, ...)     "Not a valid dynamic program"
```

read 失败是因为 argv 字符串写在了 SP **下方**，musl 第一个栈帧就把它覆盖了，路径读回空串。改成真实 Linux 布局（字符串在栈顶、指针数组在下、SP 再下）后：

```
set_tid_address
openat("/bin/busybox")  -> fd 3      正确路径
read(fd, 960)           -> 960       读到 ELF 头
mmap(len=0xf1000, R|X)  -> 0x811000  busybox 被映射
mmap(MAP_FIXED 0x8fd000)
close(fd)
brk, mprotect
```

10 个 syscall、12,688 条指令，busybox 的 ELF 头被读出并映射成功。

过程中修掉的地址空间问题：内存层不支持**跨相邻区域**访问（musl 每个 PT_LOAD 单独 mmap，memcpy 会跨边界）；`MAP_FIXED` 被忽略导致 musl 的簿记指针悬空；`brk` 返回地址却**没有真正映射内存**。

**关键修复：mmap 必须真的读文件。** 只分配零页的话，guest 看到的是空程序映像和空 dynamic 段——`dso->hashtab` 为 0，12k 条指令就崩。补上文件填充后：

| | 指令数 | syscall |
| --- | --- | --- |
| 修复前 | 12,688 | 10 |
| 修复后 | **596,728** | **585** |

musl 现在真的走完了 busybox 的 dynamic 段、解析了它的 GNU hash 表、映射了所有段，并深入运行到了自己的代码里。

**又一个掩码 bug：SMADDL 被 MADD 吞掉。**

`0x1B` 组里 `bits 23-21` 区分家族：`000` MADD/MSUB、`001` SMADDL/SMSUBL、`101` UMADDL/UMSUBL。MADD 的检查只看 `bits 28-24`，于是三种全走了 MADD，SMADDL 拿 64 位操作数做了乘法而不是符号扩展 32 位。

症状极具误导性：结果是一个**看起来合理的大偏移**，于是像是"符号索引越界"。实际上那个操作数 `0x910003FD` 本身就是 `mov x29, sp` 的指令编码。

修正后：

| | 指令数 |
| --- | --- |
| 修复前 | 596,728，然后越界崩溃 |
| 修复后 | **67,097,647**，不再崩溃 |

### 系统性排查：解码规则冲突扫描

连续六个编码 bug 形状相同——**同一前缀下用几个位选家族，规则只测了其中一部分**。逐个调试太慢，于是加了 `tools/check-decode-overlaps.py`：扫描真实二进制，报告任何被多条规则同时命中的指令。

立刻抓到两个：

| 冲突 | 命中数 | 说明 |
| --- | --- | --- |
| SMULH/UMULH 被 MADD 吞 | 31 | MADD 是 `bits 23-21 = 000`，SMULH 是 `010`；掩码只测了 bit 21 |
| SIMD 无符号偏移载入命中整数 LDR/STR 规则 | 4,130 | 靠**规则顺序**侥幸正确，现在显式测 V 位，顺序不再承重 |

musl 和 busybox 现在扫描**零冲突**。`make overlaps` 可随时复验。

当前卡点：错误的符号索引来自 `0x4006980c` 的 `ldr x4, [x6, #8]`——`x6` 在合法区域内，所以不是坏指针，而是**表的起点算错了**。

实测数据：

| 量 | 值 |
| --- | --- |
| `ld-musl` 符号表 | `0x5d28`..`0xf790`（1647 项 × 24 字节，末尾正好接 `.dynstr`） |
| `busybox` 符号数 | 401 |
| 遍历起点 `x6` | `base + 0x132b8` ← **超出末尾 0x3B28** |
| 遍历步长 | 24（`sizeof(Elf64_Sym)`，正确） |

后续每一步（`ubfx` 取高位、`cbnz` 判定、`smaddl` 符号扩展）都验证为正确。下一个问题是 `0x132b8` 这个起点从哪来。

（更早的一个卡点记录，保留作为参考：`ldr w3, [x8]`，`x8 = [dso+0x48] = 0`。）

已确认 `struct dso` 的前几个字段是正确的：

| 偏移 | 值 | 含义 |
| --- | --- | --- |
| +0x00 | 0x811000 | base（busybox 加载地址） |
| +0x08 | 0x80fda0 | 路径 |
| +0x10 | 0x900110 | **dyn = base + 0xef110 ✓ 正确** |
| +0x28 | 0x811040 | phdr |
| +0x30 | 9 | phnum ✓ |
| +0x38 | 0x38 | phentsize ✓ |
| +0x48 | 0 | 待查字段 |

所以 `dyn` 是好的，卡的是 +0x48 那个字段。

进一步追踪：`0x400698b0` 的 `br x16` 跳到 `0x4006c21c`（一个处理 argv 的内部函数），返回后才执行到 `0x400698b4`。所以这条路径是 musl **正常的**间接调用，不是控制流错误。

下一轮方向：对照 musl 1.2.5 的 `struct dso` 定义确认 +0x48 是什么字段，以及它由哪一步填充。备选是拿 qemu-user 跑同一个二进制做行为对照。

### musl 推进过程（前半程）

| 修复 | 指令数 |
| --- | --- |
| 起点 | 459（卡在 libc 初始化） |
| 零寄存器存储、CCMP | 167,494（后来发现是描述符损坏导致跑偏，非真实进展） |
| **LDRSW 误判为 store** | 1,856（修正后回到真实路径） |
| ADD/SUB extended register | 2,603 |
| CCMP 掩码收紧 + 2-source 掩码放宽 | 3,130 → 9,024 |
| MRS/MSR（TPIDR_EL0） | 9,515，**首次进入 syscall** |
| 逻辑取反形式（BIC/ORN） | 9,581 |

**关键教训**：`LDRSW` 那条 bug 让 musl 的 phentsize 字段被写成栈地址，程序因此走到一条荒谬但"能跑"的路径上，产生了 167K 指令的假进展。**指令数变多不等于更接近正确**——必须验证中间状态。

### musl 卡点的排查记录（已排除四项）

故障点：`ldr x4, [x6, #8]`，`x6 = bias + 栈地址`。

| 排查项 | 结论 |
| --- | --- |
| 启动栈不符 ABI | 排除。`argc/argv/NULL/envp NULL/AT_PAGESZ/AT_PHDR` 位置全对 |
| 宿主重定位与 musl 自重定位冲突 | 排除。跳过宿主重定位结果完全相同 |
| `CBZ` 跳转错误 | 排除（一度怀疑）。实测 `take=1 next=0x400697c0`，跳转正确 |
| SIMD 越界 | 排除。修复后行为不变 |

**已定位到的机制**：musl 在 `0x40069778` 处读自己栈帧的 `AT_BASE` 槽位（为 0），随后进入 `0x400697c0` 的 auxv 扫描循环，找 `type == AT_EXECFD(2)`；命中后算出 `x3 = x7 - auxv_value` 并跳回 `0x40069780`，在那里把 `x3` 当作偏移去加栈地址，于是得到 `bias + 栈地址`。

也就是说：**它认为 auxv 里存在 `AT_EXECFD`，而实际没有**。下一步要查的是 auxv 计数的来源（`sp+0x20` / `sp+0x30` 两个槽位），以及 musl 对 `AT_PHDR`/bias 的自举约定。

**备选路线**：如果自举这条线继续卡，就改为宿主侧完成全部动态链接（加载 busybox + ld-musl、解析符号、填 GOT），直接跳到主程序入口，绕开 musl 的 `_start`。

**两个缺口已解决，且是同一个根因**：SIMD 单寄存器访问的字节数算错。

`ldr q0` 应该读 16 字节，但代码把 `size` 和 `Q` 两个字段相乘（`(size==0?16:1<<size) * (Q?2:1)`），Q 情况下算出 32 字节。于是每次向量载入/存储都越过自己的寄存器，踩到相邻槽位。

- 向量化版本用 `stp q0, q1` 在栈上铺常量 → 32 字节存储破坏相邻数据 → checksum 恒为 0
- `volatile` 版本在第 42632 条指令 fault → 同一个越界

修正后两者都输出正确的 `checksum=58992`，cprog 恢复完整 `-O2`（含自动向量化）。

---

## 四、踩到的坑（都会在 M2 重复出现）

### 1. 条件码是「基础测试 + 取反位」

4 位条件码，高 3 位选基础测试，最低位取反。第一版只做了基础测试，于是 `B.NE` 表现成 `B.EQ`，循环一次就退出。

### 2. `Rd=31` 的含义取决于 S 位

**这个最阴。** `ADD/SUB (immediate)` 里：

- `S=0`（ADD/SUB）→ Rd=31 是 **SP**
- `S=1`（ADDS/SUBS）→ Rd=31 是 **XZR**，结果丢弃

按「Rd=31 一律当 SP」写，`CMP x0, #2` 会把比较结果写进 SP。表现是栈指针突然变成 8，下一条压栈直接越界。

### 3. `p_vaddr` 不保证页对齐

只保证与 `p_offset` 模页大小同余。真实的 lld 输出把 text 段放在 `0x210120`。必须按页向下对齐映射，数据再落到真实地址。

### 4. 段在加载期需要写权限

text 段最终是 `R+X`，但加载时要往里写内容。正确顺序是「映射可写 → 拷贝 → 降到最终权限」，不是直接按最终权限映射。

### 5. 别用 `x[31]` 表示 SP

`x[]` 只有 31 个元素。写 `x[31]` 会越过数组、踩坏相邻字段——表现是任何建栈帧的程序都莫名其妙 `stp outside mapped memory`。

### 6. 只有 immediate 形式允许 SP 操作数

`ADD/SUB` 和 logical 指令里，**寄存器形式没有 SP 变体**：`Rn=31` 必须读作 XZR。读 `cpu->x[31]` 会越界拿到垃圾值。

这条最阴，因为 `mov xD, xM` 就是 `ORR Xd, XZR, Xm`——每个寄存器移动都会写坏目标。表现是 musl 的 auxv 扫描循环永不终止，起止指针全是垃圾。

### 7. `Rm=31` 同样是 XZR

上一轮修了 `Rn` 就以为完事，漏了 `Rm`。`mov x11, xzr` 编码为 `ORR X11, XZR, XZR`——**两个操作数都是 31**。

表现：循环计数器被初始化成一个栈地址，第一次数组访问直接飞出去。这类 bug 的共同点是「读越界拿到垃圾」，而垃圾通常长得像一个合法地址，所以故障点离根因很远。

**规则**：寄存器形式的逻辑/算术指令里，Rn 和 Rm 的 31 都读作 XZR，只有 immediate 形式的 Rd/Rn 才可能是 SP。

### 8. 栈顶之上要有余量

musl 初始化时会清零 `[计算出的基址, 栈顶)`，而栈顶是开区间。最后一笔存储正好落在 SP 上。真实内核在 SP 之上留了空间；不留就会在 libc 的第一条初始化代码上 fault。

---

## 五、路线

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| **M1** | 解释器 + ELF 加载 + 基础 syscall，能跑 hello world 和递归函数 | ✅ 完成 |
| **M2** | 指令覆盖扩展到 musl 需要的水位；动态链接器；跑通 `ld-musl-aarch64.so.1` | 未开始 |
| **M3** | busybox 基础命令；`sh` 脚本、管道、重定向 | 未开始 |
| **M4** | python | 未开始 |

### M2 的真实难点

真实发行版 binary 全是动态链接的。要跑它们，M2 必须包括：

1. **SIMD/FP 指令**——musl 的 memset 用 `dup v0.16b, w1`
2. **ELF 动态段**——`PT_DYNAMIC`、符号表、重定位
3. **加载 `ld.so` 本身**——它是静态的，但要处理它给主程序做的重定位
4. **更多 syscall**——mmap、openat、read、close、fstat、brk、clone

这不是一两天的事，但每一步都能用本地测试验证。

---

## 六、许可

`e2/` 下每一行都是按 ARM Architecture Reference Manual 和 Linux syscall ABI 写的，没有复制 iSH、QEMU 或其他模拟器的代码。

因此这部分**许可自主**：可以 MIT、Apache、BSD，也可以闭源，或者双许可。这正是 E2 存在的意义。

---

## 七、怎么跑

```bash
cd e2
make test
```

需要 `cc`（C11）、`clang`（交叉编译 guest 测试程序）、`ld.lld`（链接）。没有这些工具链时，guest ELF 测试会自动跳过，其余测试照常跑。
