# E2 后端：许可自由的 Linux 运行时

状态：**M1 完成；M2 达成**（真实 busybox 端到端运行；动态链接仍在推进）
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

**先纠正一个误判**：这不是符号表遍历，是**重定位表**遍历。

| 量 | 值 | 对应 |
| --- | --- | --- |
| `DT_RELA` | `0x132b8` | 正是 `x6` 的起点 |
| `DT_RELASZ` | `2280` | 正是 `x2` 的计数 |
| 步长 | 24 | `sizeof(Elf64_Rela)` |
| `cmp x4, #0x403` | 1027 | `R_AARCH64_RELATIVE`（先前误算成 0x43） |

前段的 RELATIVE 项内容完全正确：

```
off=0xbfb30 info=0x403 type=1027 add=0xbfb60
off=0xbfb60 info=0x403 type=1027 add=0x76700
```

**真正的卡点在后段**：RELA 表末尾是 `GLOB_DAT`（type 1025）项，需要符号解析：

```
x6=0x40013b10  info=0xc400000401  type=1025   ← 符号索引 196
```

而 `0x910003fd` 正是符号解析返回的 `st_value`——所以问题在**符号表地址或索引**，不在重定位表本身。

**本轮进一步确认的事实**：

- reloc 遍历期间 `x4` 取到的是合法的 `r_info`（如 `0x66700000401` → 类型 1025、符号索引 1639），**索引 1639 < 1647，在范围内**
- 那一次 `x27 = 0`，并不指向 dso，所以符号解析走的是别的路径
- `0x910003fd` 出现在**另一条**代码路径：`0x4006a204` 的 `ubfx x4, x4, #32, #32` 之前 `x4` 已经是这个值
- `0x4006a204` 本身验证为正确：`x4 = 0xbff38` 时 `ubfx` 得 0，`cbnz` 不跳转；只有唯一一次 `x4 = 0x910003fd` 导致跳转

**根因链条已完整**（本轮定位）：

```
x28 = 0x40013ba0   ← ld-musl 自己的 .rela.plt 起点
x26 = 0x10         ← 步长 16
x20 = 0x90         ← 总长 144
```

`.rela.plt` 的项是 **24 字节**（`Elf64_Rela`），但遍历用了 **16 字节**步长——即把 `RELA` 当成了 `REL`。

musl 的判断是：

```c
do_relocs(p, laddr(p, dyn[DT_JMPREL]), dyn[DT_PLTRELSZ],
          2+(dyn[DT_PLTREL]==DT_RELA));
```

`reloc_all` 里的写法是：

```c
size_t dyn[DYN_CNT];              // 未初始化
for (; p; p=p->next) {
    if (p->relocated) continue;   // ← ldso 在这里被跳过
    decode_vec(p->dynv, dyn, DYN_CNT);
    ...
    do_relocs(p, laddr(p, dyn[DT_JMPREL]), dyn[DT_PLTRELSZ],
              2+(dyn[DT_PLTREL]==DT_RELA));
```

实测 `ldso` 的标志位（`+0xa8`）：

```
relocated=1  kernel_mapped=1
```

**所以 `dyn[]` 从未被填充**，里面是栈垃圾——`dyn[DT_JMPREL]` 恰好是 `0x40013ba0`、`dyn[DT_PLTREL]` 不是 7，这正好解释了 stride 为什么是 2。

已排除的：`ldso->dynv = 0x400bfd98` 正确，`.dynamic` 内容正确（`DT_PLTREL=7`、`DT_JMPREL=0x13ba0`、`DT_PLTRELSZ=144`），`syms = 0x40005d28` 也正确。

**但这里有个自相矛盾**：`syms = base + dyn[DT_SYMTAB]`（`0x5d28`）是**正确**的，说明 `decode_vec` 确实正确填充了 `dyn[]`；那么 `dyn[DT_PLTREL]` 也就应该是 7、stride 应该是 3。实测 stride 却是 2。

`kernel_mapped_dso` 只设 `map`/`kernel_mapped`，**不设 `relocated`**；`relocated` 由 `reloc_all` 内部（处理完后）设置。所以第一次 `reloc_all(&ldso)` 应当正常填充 `dyn[]`。

**本轮测得的输入**：`do_relocs` 的入口是 `0x40069ff8`（`stp x29, x30, [sp, #-0x30]!`），在入口处打印四个参数：

```
dso    = 0x400c2ba0   ← 正确
rel    = 0x4000f9fd   ← 未对齐，不是合法指针
size   = 0x0
stride = 2 / 25 / 51  ← 乱
```

`rel` 未对齐（`...fd`）这一点很关键：**调用方算出的地址本身就不是 8 字节对齐的**，说明那个值来自未初始化或错误的数据，而不是 `laddr(p, dyn[DT_JMPREL])` 应有的结果（那必然是页内对齐的偏移）。

调用点 `0x4006a264` 的前置指令是 `cmp w5, #2` + `cset w2, eq`，不像是 `2+(dyn[DT_PLTREL]==DT_RELA)` 的形态——**下一轮要确认这个调用点到底对应 `reloc_all` 里的哪一条 `do_relocs`**。

过程教训：本轮两次把 `bl` 的相对偏移算错（心算 26 位符号扩展），导致探针挂在错误地址上、拿到一堆假数据。这类计算应当一律用脚本而非口算。

### 方法论修正：走 interpreter 路径，不是显式链接器路径

装 `qemu-user-static` 做对照后，正常的 syscall 序列是这样的：

```
set_tid_address
brk / brk
mmap(PROT_NONE, MAP_ANONYMOUS|MAP_FIXED)   ← guard page
mprotect
getuid()
ioctl(2, TIOCGWINSZ)
dup3(1, 2, 0)
write(2, ...)
```

**没有 `openat`/`read`**——因为内核已经把主程序映射好了，再把 `ld-musl` 作为 interpreter 加载并跳转。而之前的测试跑的是 `ld-musl /bin/busybox`，即"显式调用链接器"路径，那条路要 musl 自己去 openat/read/解析目标文件，是内核永远不会走的。

改成模拟内核：busybox 载入 `0x40000000`、ld-musl 载入 `0x50000000`，`AT_PHDR`/`AT_ENTRY` 指向主程序、`AT_BASE` 指向解释器，然后跳到解释器入口。`loader` 为此增加了 `e2_load_elf_at()` 支持指定基址。

结果：musl 真的跑起来了，并给出明确诊断——

```
Error relocating /lib/ld-musl-aarch64.so.1: unsupported relocation type 786232
                                    786248
                                    786264        (每次 +16)
```

`786232 = 0xBFF38` 是 ld-musl **自己 `.data` 段的地址**，说明它把一个数据数组当成了重定位表。`dyn[]` 仍然不对，但问题从"崩溃"变成了"可诊断"。

**已知另一处缺口**：qemu 序列里的 `getuid` / `ioctl(TIOCGWINSZ)` / `dup3` 在 E2 里还没实现。

### M2 达成：真实 busybox 运行

```
BusyBox v1.36.1 (2025-11-23 14:32:18 UTC) multi-call binary.
BusyBox is copyrighted by many authors between 1998-2015.
Licensed under GPLv2. See source distribution for detailed
copyright notices.
```

未修改的 busybox 在 E2 上打印自己的横幅并 `exit 0`——**91,112 条指令，652 个 syscall**。

两个关键点：

1. **`AT_PHDR` 对 ET_EXEC 算错了**。原实现是 `e_phoff + bias`，而固定地址二进制的 `bias` 为 0，于是得到 `0x40`（文件偏移），guest 把它当指针解引用，216 条指令就 fault。program headers 位于第一个 `PT_LOAD` 内，正确地址是该段的 `vaddr` 加上 `e_phoff` 在段内的偏移（此例为 `0x400040`）。

2. **改用 Alpine 的 `busybox-static`**。与其继续啃动态链接的自举，不如先用静态 busybox 把解释器和 syscall 层在真实程序上验证一遍——把开放式调试变成了一个有输出的基线。动态链接仍是目标，其状态未变。

另外栈必须缩小：4 MB 起始于 `0x10000` 会和静态二进制的 `0x400000` 代码段重叠。

`busybox ls` 的进展与卡点：

- `0x5ac00339` 已确认是 **`rbit w25, w25`**（整数 1-source 指令，不是 SIMD），整组 RBIT/REV/CLZ/CLS 已实现
- `busybox echo hello` 可用
- `busybox ls` 卡在 getopt32 报 `NO OPT 0xff`

对 getopt32 的内部追踪（直接给 busybox 自己的代码加探针）：

| 阶段 | 结果 |
| --- | --- |
| `applet_opts` 传入 | `"^Cadi1lgnsxAkFpRQctuSXrvLHhT:w:"` ✅ |
| strlen + alloca + strcpy | 副本逐字节正确 ✅ |
| 短选项构建循环 | 逐项写入 `C,a,d,i,1,l,g,n,...`，步长 40 ✅ |
| `applet_long_options` | `"full-time\0\0\xff"` 等 ✅ |
| **互补选项扫描** | **走的表在 `0x7f0ff2c0`，而构建写在 `0x7f0ff370`** ❌ |

**根因已定位**（逐项追踪 getopt32 的两张表）：

短选项表构建**完全正确**：

```
[0..27] C a d i 1 l g n s x A k F p R Q c t u S X r v L H h T w
[28]    0xfe      ← 长选项
[29]    0xfd
[30]    0x00
```

**但 `0xff` 不在表里**——它正是第一个长选项 `full-time` 的 val。

原因在构建循环：

```asm
0x4a5c38: add  x26, x26, #0x20    ; x26 先前进 32 字节
0x4a5c50: stur w0, [x26, #-0x20]  ; 写到新项的 +0
0x4a5c58: stur w0, [x26, #-0x18]  ; 写到新项的 +8
```

而消费方读的是 `ldr w1, [x0, #0x18]`（**+24**）。追踪确认：前两个长选项读到的 val 都是 **0x0**。

于是 `0xff` 从未注册，互补选项扫描自然拒绝它。

**上一轮的两个猜测都被否定了**：

- `stur` 的立即数实际是 **-24 和 -8**（我先前手算成 -0x20/-0x18），落点正是 `+8` 和 `+24`，正确
- 项布局就是 32 字节、`val` 在 `+24`，写入没有用陈旧基址

**真正的根因：`strlen` 在 busybox 里对 `"full-time"` 返回 35（应为 9）。**

追踪证据：

```
[strlen] pc=0x4a5c34 x22=0x509301 x0=...        ← 调用前
[strlen] pc=0x4a5c40 x22=0x509301 x0(ret)=35    ← 返回 35
[strlen] pc=0x4a5c40 x22=0x509327 x0(ret)=5     ← "color" 返回 5，正确
```

`35` 恰好是到下一个 `\0`（`group-directories-first` 结尾）的距离——**它跳过了中间的 `\0\0`**。

于是 `x2 = optstr + 35 + 1`，`val` 读到的是第二个长选项的 `0xfe`，第一个长选项的 `0xff` 从未注册，互补选项扫描自然拒绝它。

**但把 musl 的 strlen 算法原样搬到 E2 上跑，结果完全正确**：

```
E2 : 9,8,7,6
期望: 9,8,7,6
```

（`HASZERO(x) = (x - ONES) & ~x & HIGHS` 单独验证也与主机一致。）

**根因找到了：logical immediate 的 `Rn=31` 被当成 SP 读取。**

`mov xD, #imm` 编码为 `ORR Xd, XZR, #imm`，`Rn=31` 应为 **XZR**。但实现里用了 `e2_read_sp`，于是每条常量加载都算成 `sp | mask`。

追踪链条：

1. getopt32 用 `mov x4, #0xfefefefefefefefe` 构造 `ONES`
2. 该常量实际得到 `0xfefefefefffffeff`（SP 被 OR 进去）
3. `HASZERO` 因此漏掉内嵌的 NUL
4. `strlen` 一路数到下一个字符串结尾，返回 35
5. 长选项的 `val` 读到第二项，`0xff` 从未注册
6. 互补选项扫描拒绝 `0xff` → `NO OPT`

修复并验证：同一指令序列现在得到 `0xfefefefefefefefe`。

`busybox ls` 从 15,401 条指令推进到 **99,917** 条。

### 下一个卡点：分配器空闲链表成环

`ls` 现在停在 `0x485500`：

```asm
0x4854fc: mov w0, #0                  ; i = 0
0x485500: str x19, [x22, w0, uxtw #3] ; array[i] = node
0x485504: ldr x19, [x19, #0x10]       ; node = node->next
0x485508: cbz x19, done
0x48550c: add w0, w0, #1
```

数组大小算得对（`w21 = 17`），但**链表成环**，`x19` 永不归零：

```
i=5: 0x10000690 -> 0x100007c0
i=6: 0x100007c0 -> 0x10000700
i=7: 0x10000700 -> 0x10000690   ← 回到 i=5
```

于是 `w0` 一路涨到 1738，`array[i]` 写出映射外。

**环的成因已定位到字节级。**

链表本身建得完全正确——19 个节点，`next` 依次相连，最后归 NULL：

```
new=0x100000a0 next=0x0
new=0x10000110 next=0x100000a0
...
new=0x10000890 next=0x10000820
```

破坏发生在之后。对 `0x100007d0`（节点 `0x7c0` 的 `next` 字段）下写陷阱，捕获到四次写入：

```
pc=0x4004c0  = 0x0                    ← PLT 清零
pc=0x485550  = 0x10000750             ← 正确的 next
pc=0x4b93ec  = 0x00 (1 字节)          ← 把低字节改成 0！
```

`0x750` 被改成 `0x700`，而 `0x700 -> 0x690 -> 0x7c0`，环就此闭合。

`pc=0x4b93ec` 是 `strb wzr, [x6, w1, sxtw]`，memset 的尾字节对齐处理：

```
x6 = 0x100007dc   w1 = -12   →   写 0x100007d0
```

覆盖范围是 `[0x770, 0x7dc)`——**清零区域越界伸进了相邻节点的 `next` 字段**。

**进一步定位：不是 memset，是 malloc 返回了重叠的块。**

对 `0x100007d0` 下陷阱抓到的那条 `strb wzr` 属于 **malloc 写块头部**（`strb [x0,#-3]` / `sturh [x0,#-2]` / `stur [x0,#-4]` 这一组），不是 memset。

追踪分配序列（`xmalloc` 包装在 `0x405c5c`，`0x4b8fe0` 是真正的 malloc）：

```
size=0x60 -> 0x100007c0    ; 块区间 [0x7c0, 0x830)
size=0x60 -> 0x10000690
size=0x60 -> 0x10000700
size=0x60 -> 0x10000770    ; 块区间 [0x770, 0x7e0) —— 和上一块重叠
```

**分配器把已占用的区间又分了一次。** 0x60 的请求实际占 0x70（含 16 字节头部），所以 0x770 那块覆盖到 0x7e0，而 0x7c0 那块的头部正落在 0x7c0（已分配区内）。

块重叠最终让 0x7c0 节点的 `next` 字段被新块的头部写坏，形成环。

**本轮排除了三个嫌疑**（都用独立测试程序验证，非推理）：

| 嫌疑 | 验证方式 | 结果 |
| --- | --- | --- |
| `LDXR`/`STXR` 的 CAS 语义 | 手写 CAS 循环：匹配时写入、不匹配时不写 | **exit 0，正确** |
| `a_ctz_32`（分配索引靠它） | 测 `CLZ`/`RBIT` 在 0、1、0x80000000、0x100 等输入的输出 | **exit 0，正确** |
| `mmap` 返回地址不连续 | trace 两次匿名映射 | **`0x10000000` 与 `0x10002000`，连续** |

mallocng 的分配路径：`avail_mask` 位图 → `first = mask & -mask` → `idx = a_ctz_32(first)`。位图、ctz、CAS 都已确认无误。

**但块重叠仍然存在**，而且是关键异常：

```
0x60 请求占用 0x70（含 16 字节头部）
0x100007c0 块 → [0x7c0, 0x830)
0x10000770 块 → [0x770, 0x7e0)     ← 与上一块重叠
```

两个都在同一个 group（`[0x10000000, 0x10002000)`）内，相差 `0x50`，而 `0x50` 不是 `0x70`（slot 大小）的倍数——**说明其中至少一个不是合法的 slot 起点**。

**查明了：两个"重叠块"其实是同一个 slot 的两次分配。**

mallocng 的参数（`meta.h`）：

```c
#define UNIT 16
#define IB 4
stride = UNIT * size_classes[sizeclass]     // 0x60 请求 → size_classes[6] = 7 → stride 0x70
```

而 `enframe()` 里有一个 **cycle offset** 机制：

```c
int off = (p[-3] ? *(uint16_t *)(p-2) + 1 : ctr) & 255;
if (off) {
    *(uint16_t *)(p-2) = off;
    p[-3] = 7<<5;          // 标记"有偏移"
    p += UNIT*off;         // 返回地址挪到 slot 内部
    p[-4] = 0;
}
p[-3] = idx;
```

把这个代进去看：

| 返回地址 | 相对 slot `0x700` | off |
| --- | --- | --- |
| `0x10000770` | `+7×16` | 7 |
| `0x100007c0` | `+12×16` | 12 |

**但上一轮的推论错了**——直接读每个块头部的 `slot` 与 `off` 字段后，真相不同：

```
0x10000030 slot=0 off=0
0x100000a0 slot=1 off=7
0x10000110 slot=2 off=14
0x10000180 slot=3 off=21
0x100004e0 slot=0 off=0     ← 新 group
...
0x100007c0 slot=3 off=21
0x10000690 slot=0 off=0
0x10000700 slot=1 off=7
0x10000770 slot=2 off=14
0x100007e0 slot=3 off=21
```

**每个 group 只有 4 个 slot**（0-3），用满就换新 group。所以 `0x7c0` 是 **slot 3**、`0x700` 是 **slot 1**——**不同 slot，不存在重复分配**。

我按 `stride 0x70` 把 `0x7c0 = 0x700+0xc0` 解释成"单 slot 内部偏移"，这一步跨了多个 slot，是错的。

同时 `avail_mask` 的变化也已被观测并确认正确：

```
meta=0x20001018 avail=0xe → 0xc → 0x8 → 0x0
```

每次取最低位并清除。

**所以内存分配层面是干净的。** 环来自链表指针本身的错误写入。

对 `0x100007d0` 重新下陷阱，三次写入：

```
pc=0x4004c0  8B = 0x0            PLT 清零
pc=0x485550  8B = 0x10000750     正确的 next
pc=0x4b93ec  1B = 0x0            破坏者
```

`0x4b93ec` 是 malloc 内部的 `strb wzr, [x6, w1, sxtw]`，`x6=0x7dc`、`w1=-12`，落点 `0x7d0`。

同时确认 **`free()` 从未被调用**（唯一匹配 `0x4b7ed4` 的参数是 meta 地址 `0x200010b8`），所以 `0x7c0` 块一直存活。

**这里出现一个矛盾。** `enframe()` 里：

```c
size_t slack = (stride-IB-n)/UNIT;     // (0x70 - 4 - 0x6c) / 16 = 0
int off = (p[-3] ? *(uint16_t *)(p-2) + 1 : ctr) & 255;
if (off > slack) { ... off &= m; ... }
if (off) { p += UNIT*off; ... }        // slack=0 时 off 必须为 0
```

**按参数算 `slack = 0`，`off` 必须为 0**，但实测块头里 `off` 是 14/21 这样的非零值——`0x770` 块的清零范围 `[0x770, 0x7dc)` 因此越过 slot 边界（slot 2 结束于 `0x7a0`），正好压到相邻 slot 的 `next` 字段。

**追踪结果：`off` 最终被清成 0，enframe 的逻辑是对的。**

```
[enframe] pc=0x4b937c off=0x1 m=0xc
[enframe] pc=0x4b938c off=0x0 m=0x0     ← off &= m 之后归零
[enframe] pc=0x4b93a4 off=0x0 m=0x0
```

**而且从返回地址反推出了真实参数**，推翻我前面几轮的假设：

| 观测 | 值 |
| --- | --- |
| slot 返回地址 | `0x690, 0x700, 0x770, 0x7e0`（间隔 `0x70`） |
| 下个 group 的 slot 0 | `0x820` |
| group 跨度 | `0x820 - 0x690 = 0x190` |
| **stride** | **`0x190 / 4 = 0x64 = 100`** |

**`stride = n(96) + IB(4) = 100`**——不是我之前假设的 `UNIT * size_classes[6] = 0x70`。这正好让 `slack = 0`，与「off 必须为 0」自洽。

也就是说：`p[-2]` 存的不是 cycle offset，而是 **`(p - storage)/UNIT`**（slot 位置标记，`enframe` 末尾那句 `*(uint16_t*)(p-2) = (p-g->mem->storage)/UNIT`）。我之前把它读成 offset 才得出「越界」的结论。

**所以块范围和 slot 边界都没有问题。** `[0x770, 0x7d0)` 落在 slot `[0x770, 0x7d4)` 内。

**但重新核对完整序列后，又出现了新的不一致。**

分配返回地址序列：

```
0x10000030  0x100000a0  0x10000110  0x10000180
0x100004e0  0x10000550  0x100005c0  0x10000630
0x10000670  0x100006e0  0x10000750  0x100007c0
0x10000690  0x10000700  0x10000770  0x100007e0
0x10000820  0x10000890
```

多数相邻间隔是 `0x70`，但有两处是 **`0x40`**：

- `0x10000630 → 0x10000670`
- `0x100007e0 → 0x10000820`

`0x40` 不是 `0x70`（stride）的倍数，也不是 `0x64` 的倍数。而按 `enframe` 末尾那句

```c
*(uint16_t *)(p-2) = (size_t)(p-g->mem->storage)/UNIT;
```

`p[-2]` 应当**总是**被覆盖成 slot 位置标记，不该残留 cycle offset。所以同一个 slot 里出现两个返回地址这件事，本该被 `avail_mask` 阻止。

**现有的三个数字互相矛盾**：

| 假设 | 推出的 stride | 与 `+0x40` 间隔是否相容 |
| --- | --- | --- |
| `UNIT * size_classes[6]` | `0x70` | 否 |
| `n + IB` | `0x64` | 否 |
| 从 group 跨度反推（`0x190/4`） | `0x64` | 否 |

**直接读块头后，参数完全对上了，问题定位到 group 尺寸。**

```
ret=0x10000690  slot=0  off=0    calc=0
ret=0x10000700  slot=1  off=7    calc=7
ret=0x10000770  slot=2  off=14   calc=14
ret=0x100007e0  slot=3  off=21   calc=21
ret=0x10000820  slot=0  off=0    calc=25   ← 新 group 的 storage
```

`off` 与 `(p − 0x690)/16` **逐项精确吻合**，所以：

| 量 | 值 |
| --- | --- |
| group 起点（storage） | `0x10000690` |
| 下一个 group 起点 | `0x10000820` |
| **group 跨度** | **`0x190` = 400 字节** |
| slot 数 | 4（索引 0-3） |
| stride（`UNIT × size_classes[6]` = `16 × 7`） | `0x70` = 112 字节 |
| **4 个 slot 需要的空间** | **`4 × 112` = 448 字节** |

**group 只有 400 字节，却要装 448 字节的 slot。少 48 字节。**

slot 3 落在 `0x7e0`，占 `[0x7e0, 0x850)`——而 group 在 `0x820` 就结束了，**越界 0x30**。越界部分正好覆盖下一个 group 的 storage 头部，也就是链表节点的 `next` 字段。

**在 `enframe` 内部读参数后，块尺寸也确认了**：

```
0x4b9318: add x6, x0, x6
    x0 = stride*idx + storage   （slot 起点）
    x6 = p + n，其中 n = 0x6c = 0x70 - IB
```

也就是说块的实际占用是 `n + IB = 0x70`，与 stride 一致。slot 3 在 `0x7e0`，块结束于 `0x7e0 + 0x6c = 0x84c`。

而 group 在 `0x820` 结束——**越界 `0x2c`**。

**问题收敛到 group 的容量。** `alloc_group()` 里：

```c
size_t needed = size*cnt + UNIT;          // 112*4 + 16 = 464
needed += -needed & (pagesize-1);         // 对齐到 4096
m->maplen = needed>>12;                   // = 1
```

按这段代码，group 应该是 **4096 字节**，足以容纳 448 字节的 slot。但实测只有 **400 字节**。

**dump 内存后澄清了一点**：`0x820` 处的确是正常节点数据：

```
0x10000820: 0000000010000442 0000000010000440
0x10000830: 00000000100007e0 000041ed00000001    ← next=0x7e0
```

**但它同时也是某个 group 的第一个 slot**——因为 `enframe` 只写 4 字节头（`p[-4..p-1]`），第一个 slot 的 `off = (p−storage)/16 = 0` 就意味着 `p == storage`。所以"group storage"和"节点数据"是同一块内存，不矛盾。

**矛盾仍在别处**：`alloc_group` 算出 `needed = 464 → 对齐到 4096`，而相邻两个 storage（`0x690` 与 `0x820`）只隔 400 字节。

**读 meta 后确认：两个 group 都是嵌套的。**

按 `get_meta` 的算法（`base = p − offset×16 − 16`，`meta = base->meta`）反推：

```
[g0] base=0x10000680  meta=0x20001158  last_idx=3  sizeclass=6  maplen=0
[g1] base=0x10000810  meta=0x200011a8  last_idx=3  sizeclass=6  maplen=0
```

`maplen = 0` 表示它们不是独立 mmap，而是**从一个外层 group 里分出来的 slot**（`alloc_group` 的 else 分支）：

```c
int j = size_to_class(UNIT+cnt*size-IB);      // 4 个 0x70 slot 需要 448 → class?
int idx = alloc_slot(j, UNIT+cnt*size-IB);
p = enframe(g, idx, UNIT*size_classes[j]-IB, ctx.mmap_counter);
```

所以外层要为每个嵌套 group 提供 `UNIT*size_classes[j]` 字节。4 个 `0x70` slot 需要 **448 字节**，而 `size_classes` 里 448/16 = 28 没有对应项，会被归到 31（`31×16 = 496`）——**外层 slot 至少该有 496 字节**。

**但实测两个嵌套 group 只隔 400 字节。**

也就是说：外层 group 分配给第二个嵌套 group 的 slot 起点，比第一个 nested group 需要的空间还小——**这正是重叠的机制**。

**按源码手算 `size_to_class(448)`：**

```c
n = (448 + IB - 1) >> 4 = 451 >> 4 = 28;
if (n < 10) return n;          // 不成立
n++;                            // 29
int i = (28 - a_clz_32(29)) * 4 + 8;   // clz32(29)=27 → i = 12
if (n > size_classes[i+1]) i += 2;     // 29 > size_classes[13]=20 → i = 14
if (n > size_classes[i]) i++;          // 29 > size_classes[14]=25 → i = 15
return 15;                              // stride = 31*16 = 496
```

**正确结果应当是 class 15 → stride 496 字节。**

**但实测两个嵌套 group 只隔 400 字节**，而 `400 = 25×16` 正是 **`size_classes[14]`**。

也就是说：外层在给嵌套 group 选 slot 时用了 **class 14（400 字节）**，而实际需要 **448 字节**——400 < 448，溢出 48 字节，正好压在下一个嵌套 group 的头部。

`clz` 本身已经在前面单独验证过（各种输入全对），所以嫌疑落在 `size_to_class` 里那两条比较：

```c
if (n > size_classes[i+1]) i += 2;
if (n > size_classes[i]) i++;
```

**把 `size_to_class` 原样编译到 E2 上跑，确认它确实返回 14：**

```
E2  : 400:14 404:14 ... 448:14 ... （全部 14）
host: 400:15 404:15 ... 448:15 ... （15，到 496 才变 16）
```

**但两条嫌疑都被排除了**：

1. **比较与数组读取**——最小复现（`n=29`，读 `sc[13..15]`，跑那两条 `if`）在 E2 上给出 `20,25,31|1,1,0,1,14,15`，与主机**逐字符一致**。
2. **`CLZ` 指令**——用 `__builtin_clz`（真的编译成 `clz w`）测 1/2/5/29/100/0x80000000/0x7fffffff，结果 `31,30,29,27,25,0,1`，且 `i=(28-clz)*4+8` 得 `27,12`，**全对**。

**所以两个部件单独都对，拼起来却错。**

**把完整 48 项表搬进去后，错误依旧，但范围收得更紧了。**

```
E2  : 20,25,31|400:14 448:14 496:16 500:16
host: 20,25,31|400:15 448:15 496:16 500:16
```

`400`/`448` 错，`496`/`500` 对。差别在所需进位次数：`n=26` 要连进两次（`26>20`→14，`26>25`→15），`n=32` 一次都不用进。

**但接下来两步又把两个候选都排除了：**

1. **两条 `if` 在独立程序里完全正确**——硬编码 `i=12; n=26`，结果 `1,14,1,15|1,1,15`，与主机一致。
2. **把整段中间值打印出来，全部正确**——`25,26,27,12,20,14,25,15`，最终 `i=15`，与主机逐个一致。

**这两次的唯一差别是：错的版本把逻辑放在 `static int size_to_class(size_t n)` 函数里，对的版本内联在 `_start` 中。**

**但"函数调用"这个解释也站不住**：反汇编 `stc2.elf` 后发现**没有任何 `bl` 指令**——`-O2` 把 `size_to_class` 完全内联了。所谓"包成函数就错"并不成立。

**`.bss` 清零也验证过是正确的**：声明 `static int`、`static long[64]`、`static char[4096]` 并在启动时求和，E2 输出 `0,0,0,0`，与主机一致。

**现在最可疑的是探针自己的值。**

给 `stc2` 加中间量记录后，输出是：

```
E2  : 20,25,31|400:14 448:14 496:16 500:16 |31,26,0,0,20,25,14
```

这些数字**互相矛盾**：

- `dbg_n=31`、`dbg_clz=26` 是**无条件**存储，所以反映的是**最后一次**调用（req=500：`(500+3)>>4 = 31`，`++` 后 32，`clz(32)=26`）——自洽。
- `dbg_c1/dbg_c2/dbg_i` 只在 `if (!dbg_seen)` 里存，应当是**第一次**调用（req=400）。
- 但 req=400 时 `n=26`，`c1 = (26 > size_classes[13]=20)` 必须为 **1**，实测 **`c1=0`**。
- 而 `i=14` 又与 `c1=0` 不相容（`c1=0` 时 `i` 应停在 12）。

**但"存储串位"这个推断也被推翻了。**

对 8 个相邻的 `static int` 写 100..107、对两个 `static long` 写 `0x1122334455667788` 和 `0x99AABBCCDDEEFF00`，再全部读回：

```
E2: 100,101,102,103,104,105,106,107|287454020,1432778632,2578103244,3723427584
```

逐项与正确值核对，**全部一致**：

| 字段 | 期望 | E2 |
| --- | --- | --- |
| `0x11223344` | 287454020 | 287454020 |
| `0x55667788` | 1432778632 | 1432778632 |
| `0x99AABBCC` | 2578103244 | 2578103244 |
| `0xDDEEFF00` | 3723427584 | 3723427584 |

（这一轮我先误记了期望值，重算后 E2 是对的。）

**所以 `.bss` 清零正确、`static` 存取正确、`clz` 正确、比较与查表正确、`size_to_class` 内联后仍返回 14、而无函数调用可查。**

已排除的：`LDXR/STXR`、`CLZ/RBIT`、`mmap` 布局、`.bss` 清零、`static` 存取、`unsigned→size_t` 扩展、两条 `if` 的比较、48 项表的读取。

## 根因：`CSINC` 被当成 `CSEL` 解码（M2 达成）

对比反汇编后看出编译器把两条 `if` 编成了 `csel`(lo) + `cset`(hs)：

```asm
cmp  x3, x6
csel w6, w0, w18, lo
...
cmp  x3, x6
cset w3, hs
```

**问题出在解码掩码。**

`CSEL / CSINC / CSINV / CSNEG` 这四种指令由 **bit 30 与 bit 10（op2）** 共同选择，而不是 bits 30-29——**bit 29 是 S 字段，在这一组里恒为 0**。

原实现用 `BITS(insn, 30, 29)`，导致：

| 指令 | 正确选择位 | 被误判为 |
| --- | --- | --- |
| CSEL | b30=0, b10=0 | CSEL ✓ |
| **CSINC** | b30=0, **b10=1** | **CSEL** ✗ |
| CSINV | b30=1, b10=0 | CSEL ✗ |
| CSNEG | b30=1, b10=1 | CSNEG? |

而 `cset` 正是 `csinc Rd, ZR, ZR, invert(cond)`——被当成 `csel` 后返回 `Rm`，也就是 `XZR`，**恒为 0**。

**于是 guest 里所有比较产生的布尔值都是 0。** 这就是 `size_to_class` 里两条 `if` 的增量全部丢失的原因：外层分配器因此选小了 size class，嵌套 group 重叠 48 字节，压到链表节点的 `next` 指针上——最终形成那个卡了十几轮的循环链表。

修复后：

```
cset / 全部 14 个条件码        与主机一致
size_to_class(400/448/496/500)  = 15/15/16/16   与主机一致
busybox ls                      列出全部 17 个目录，exit 0
                                104,206 条指令，57 个 syscall
```

顺带实现了 `ls` 接下来需要的 **`EXTR`**（位段提取）——bit 23 的 `100111` 把它与位域组的 `100110` 区分开，且 `Rn` 可以不同于 `Rm`，所以它不是单纯的 rotate。

`make overlaps` 对 musl 与 busybox 均报 `no overlaps`。

**M2（syscall 层 + busybox 基础命令）达成。**

另一个纠正：被拒绝的字符是 **`0xff`**，不是空格——终端把它渲染成了不可见字符，导致前一轮在字符串里找一个根本不存在的空格。

步长错位后，`ldr x4, [x28, #8]` 读到的是 `0x910003fda9bf7bfd`——两条指令编码的拼接（`stp x29,x30` + `mov x29,sp`），因为指针滑进了 `.text`。这就是污染来源。

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
