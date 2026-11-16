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

## M3：sh 脚本、管道、重定向

进入 M3 后先修了一个真实的 MOVI 缺陷，再补上 shell 需要的文件原语。

**MOVI 的 `cmode` 表几乎全错。** 用汇编器逐项反查（不靠记忆），得到的真实映射与直觉相反——`cmode 0xE` 才是字节通道，`0x0` 是 32 位通道：

| cmode | 元素 | 移位 |
| --- | --- | --- |
| 0x0 / 0x2 / 0x4 / 0x6 | 32 位 | 0 / 8 / 16 / 24 |
| 0x8 / 0xA | 16 位 | 0 / 8 |
| 0xC / 0xD | 32 位 | msl 8 / 16 |
| 0xE (op=0) | 8 位 | — |
| 0xE (op=1) | 64 位 | — |

旧代码把 0x0 当字节通道、0x2 当 16 位、0x6 当 64 位，所以任何非全零的向量常量都是错的。

**另一个标志位错误**：musl 的 aarch64 `fcntl.h` 里 `O_DIRECTORY` 是 `040000`（0x4000），而 `0x20000` 是 `O_LARGEFILE`。把后者当成 `O_DIRECTORY` 让所有重定向以 `EINVAL` 失败。

新增的文件操作（全部可选、空指针检查，保持解释器不含策略）：

- `pipe2`
- `dup3`
- `fcntl`（`F_DUPFD` / `F_DUPFD_CLOEXEC` / FD/FL 查询）

验证结果：

```
sh -c 'echo hello'                        hello
sh -c 'echo a; echo b'                    a, b
sh -c 'x=5; echo $x'                      5
sh -c 'for i in 1 2 3; do echo $i; done'  1 2 3
sh -c 'if true; then echo yes; fi'        yes
sh -c 'echo hi > /tmp/o.txt'              文件内容为 hi，exit 0
```

**fork / exec / wait 已接通**（库内 `e2_memory_clone` + `e2_machine_clone`，宿主侧 16 槽进程表与协作式调度）。

一个真实的架构错误也被修掉：**fd 表原本是全局的**。fork 必须复制描述符表、exec 必须保留它，而全局表让子进程的 `dup3` 直接改写了父进程的描述符。现在每个进程拥有自己的表，`fork` 用 `dup()` 逐项复制——子进程可以独立关闭，管道数据仍然共享，这正是 shell 依赖的 EOF 语义。

当前状态：

```
ls                        列出根目录
echo hi > file            文件内容为 hi
cat file                  hi
echo x | cat              x（cat 从管道读、写到终端）
echo hello                hello
```

**根因找到了：`execve` 用错了指针类型。**

`p_execve` 里写的是 `e2_memory_read(m, ...)`，而接口要的是 `e2_memory *`——传进去的是 `e2_machine *`。于是**每一次读都失败**，参数个数始终为 0，子进程拿着空参数运行 applet。这解释了为什么独立的 `cat file` 正常、而 `sh -c 'cat file'` 什么都不输出。

改成 `&m->mem` 之后，外部命令保住了参数，**管道数据也真正流动起来**：

```
sh -c 'ls | head -3'        bin, dev, etc
sh -c 'cat /tmp/o4.txt'     hi
sh -c 'echo hi > f; cat f'  hi
```

顺带补了两条 shell 需要的指令：

- **FMOV**（通用寄存器 ↔ 标量浮点）`1E270000` / `1E260000` / `9E270000` / `9E260000`
- **SHLL / SSHLL / USHLL**——printf 在整数通道上运算前的加宽移位。源宽度由 `immh` 的最高置位决定（1→8, 2→16, 4→32, 8→64），移位量是 7 位 `immh:immb` 减去该宽度；第一版按猜测写的，是错的，按这条规则重写。

**仍失败**：`echo abc | wc -c` 无输出、`echo hi | grep h` 停在 `0x4c4408` 的 unscaled load。

### SHLL 的掩码曾经是错的

`0x9F3E0400 == 0x0F200400` 根本匹配不到它想选的指令：`sshll v1.2d, v0.2s, #4` 的编码是 `0x0F24A401`，`immb` 字段泄漏进了比较，所以带非零移位的形式从未被识别。

从实际编码反推固定位：bit31=0、bits28-23=`011110`、opcode bits15-10=`101001`，得到掩码 **`0x9F80FC00 == 0x0F00A400`**。移位 0 与移位 4 两种形式现在都能解码。

元素宽度由 `immh` 的最高置位决定（1→8, 2→16, 4→32, 8→64），移位量是 7 位 `immh:immb` 减去该宽度——已用 `0x0F20A400`（esize 32, shift 0）和 `0x0F24A401`（esize 32, shift 4）两条反汇编核对。

**这一改动没有解决 grep**：它的 fault 在 `0x4c4408`，位于自身记录扫描循环里（每次 `x1 += 56`，直到 `w3` 的 bit31 置位），与 SHLL 无关。

### LD1/ST1 补上后，M3 大面积可用

`memcpy` 的宽路径用 `ld1 {v0.16b, v1.16b}, [x]` / `st1 ...` 一次搬 32 字节，任何拷贝字符串的 applet 都会撞上。该组指令的 bits 14-13 = `01` 表示两个寄存器，Q 决定元素是 16 还是 8 字节。

补上后，一轮广度验证全部通过（12/12，无 fault）：

```
ls / cat / wc -c / wc -l / sed -n 1p / grep -F / head -1 / sort / echo
cat file | grep -F pattern
```

### M3 完成：描述符生命周期是最后一道坎

管道不通的根因有两处，互相叠加：

1. **fd 0/1/2 被排除在引用计数之外**——而 shell 恰恰把重定向落在这些描述符上，计数永远配不平。
2. **进程退出时没有释放描述符**——于是管道的写端一直被引用，读端等一个永远不会到来的 EOF。

正确模型是：`fork` 共享打开文件描述并递增引用计数（不是 `dup()`），`close` 递减，`execve` 关闭所有标记 `FD_CLOEXEC` 的描述符——这正是 shell 丢弃不想让子进程保留的那一端的方式。`F_DUPFD_CLOEXEC` 设置该标志，而普通 `F_DUPFD` 不设置，因为 shell 依赖后者跨越 exec 来恢复保存的 stdout。

十四种 shell 形式全部验证通过、输出正确：

```
echo hello / x=5; echo $x / for-do-done / if-then-fi / case / 函数定义
echo hi > f; cat f            hi
echo hi | cat                 hi
printf 'a\nb\n' | wc -l       2
ls | wc -l                    17
ls | head -2                  bin, dev
ls | sort | head -3           bin, dev, etc
cat f | grep -F h             h
printf 'x\ny\n' | sort        x
```

产生这些结果的测试台已收入仓库：`tools/rune.c`（`make tools/rune`），各调试开关有注释。顺手修正了它与 `e2_file_ops` 不一致的函数签名——**这类签名漂移正是先前静默丢弃 exec 参数的那种错误**。

### 仍未解决

| 现象 | 定位 |
| --- | --- |
| `grep PATTERN`（不带 `-F`）在 `0x4c4408` fault | **正则引擎**内部；`grep -F` 正常，说明不在管道或文件层 |
| `while read l; do ...; done < file` 无输出 | `read` 内建 + stdin 重定向，尚未定位 |

`ioctl(0, ...)` 在管道上返回 `ENOTTY`（-25）是**正确**行为，不是缺陷。

### 验证方法上的一个教训

给 SHLL 写的第一版测试用 `mov v0.s[0], w9` 布置数据，但那属于 INS（元素插入）指令，尚未实现，于是测试拿到的 v0 是垃圾、结论不可信。**测试本身依赖未实现指令时，测出来的失败不能用来判断被测指令对不对。**

改用「数据放内存 + `ldr q0` 载入」之后，三种形式全部与期望吻合：

```
sshll v1.2d, v0.2s, #0   ->  1, -1
sshll v1.2d, v0.2s, #4   -> 16, -16
ushll v1.2d, v0.2s, #0   ->  1, 4294967295
```

所以**加宽移位本身一直是对的，错的只有掩码**。这条验证已加入 `make test`（`tests/guest/shll.elf`）：测试加载 guest、捕获 `write` 输出并比对字符串，共 58 项检查。

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

## M4：动态链接打通，Python 已能跑起来

**动态链接不再是障碍。** 测试台现在按内核的方式加载程序：读 `PT_INTERP`，把主程序映射到 `0x40000000`、解释器映射到 `0x50000000`，在新栈上摆好 argv/envp/auxv（主程序用 `AT_PHDR`/`AT_ENTRY`，解释器用 `AT_BASE`），然后从解释器入口进入。`execve` 走同一条路径，所以 shell 也能启动动态程序。

对 Alpine 的动态 busybox 验证：

```
busybox ls          列出全部 17 个目录，exit 0，272,547 条指令
busybox echo hi     hello-dynamic
```

**顺带修掉一个编码错误**：FMOV 的 64 位形式 `type=01`，即 `fmov d0, x0` 是 `0x9E670000`、`fmov x0, d0` 是 `0x9E660000`。之前写的 `0x9E27`/`0x9E26` 是 32 位的 type 字段却把 sf 置了 1。

### Python 的进展

装了 Alpine 的 `python3.12`（动态 PIE）及其依赖库，直接跑：

```
python3.12 -c "print(1+1)"   5,926,291 条指令后停在 fmov s1, #0.5
```

也就是说 **M4 不再卡在架构上，只卡在具体指令**。一路遇到的缺口依次补掉：

| 指令 | 作用 |
| --- | --- |
| `fmov d0, x0` | 通用寄存器 → 浮点寄存器（64 位编码修正） |
| `ld1 {v0.d}[1], [x0]` | 单元素载入一个 lane（`index = (Q<<1)\｜S`，L 位在 bit 22） |

### 浮点相关指令（进行中）

| 指令 | 状态 |
| --- | --- |
| `fmov s`（浮点立即数） | **已验证正确**，见下 |
| `ucvtf` / `scvtf`（通用寄存器 → 浮点） | 已实现，宿主 `float`/`double` 转换（IEEE 最近偶数） |
| `fmul/fdiv/fadd/fsub/fmax/fmin/fnmul` | 已实现，宿主浮点运算（含 NaN 与有符号零） |
| `fcmp` / `fcmpe` | 已实现，按架构规定设置 NZCV（含无序情形） |
| `ucvtf d0, d0`（SIMD 标量形式） | **未实现**，编码族与通用寄存器版不同 |

**关于浮点立即数的结论修正。** 上一轮说「guest 回读测试不一致，故不宣称可用」——这次绕过 guest，直接驱动 `e2_step` 执行单条指令：

```
fmov s0, #0.5   -> 3F000000      fmov s0, #2.0   -> 40000000
fmov s0, #1.0   -> 3F800000      fmov s0, #-0.5  -> BF000000
```

**四项全部正确，实现一直是对的，是那个 guest 测试有缺陷。** 这四条现在作为单指令检查进入 `make test`（共 63 项）——这种检查方式没有编译器代码生成夹在中间，骗不了人。

这已经是本项目第三次「测试本身不可信」：INS 那次、`m` vs `&m->mem` 那次、以及这次的浮点回读。规律很清楚：**被测路径之外的任何未验证环节，都会让测试结论失去意义。**

`fmov` 立即数的解码掩码也修正过：该组模式是 `0xFF201FE0 == 0x1E201000`，不是 `0x1E200000`。

### 后续一轮补齐的四组指令

| 组 | 说明 |
| --- | --- |
| SIMD 标量 `SCVTF`/`UCVTF` | `U` 在 bit29、`sz` 在 bit22，掩码 `0xDF3FFC00 == 0x5E21D800` |
| `FCVTZS`/`FCVTZU` | C 的 float→int 转换。架构上越界值**饱和**、NaN 映射为 0，所以显式做范围钳制，而不是依赖宿主转换（那里是未定义行为） |
| Advanced SIMD three-same | 逻辑子组由 **size 字段**选操作（0 AND / 1 BIC / 2 ORR / 3 ORN），add/sub/mul 由 (U, opcode) 选 |

### Python 的推进曲线

```
5,883,020   动态链接打通后
5,926,298   补完 fmov 立即数与整数→浮点
5,929,498   补完浮点二元运算与比较
6,354,390   补完 SIMD 标量转换与 float→int
8,322,174   补完 SIMD 三段式整数运算
```

**停止原因的性质变了**：不再是「未实现指令」，而是 `load outside mapped memory`，指令地址 `0x1016108C`。

### 当前卡点（诚实记录）

```
insn=0xf9400c01  ldr x1, [x0, #0x18]      x0 = 0
```

**空指针解引用**。检查结果：

- 尾部只有三次 `openat` 返回 `-2`（ENOENT），是 Python 正常的路径探测
- **没有** `mmap`/`brk` 失败，所以不是内存耗尽
- 因此这个 0 来自更早的计算——很可能是某条已实现指令在特定输入下算错，或某处查找应当命中却没有

这个位置在 `0x1016108C`，属于运行期加载的模块。要定位它需要：在此前若干百万条指令里追踪某个值如何变成 0，工作量以轮计，本轮预算不允许。

**关于测试可信度的又一次教训**：浮点立即数的展开公式在独立计算下完全正确，但 guest 回读测试给出不一致的结果。本项目已经多次因为「测试本身依赖未验证的路径」而得到错误结论，所以这一次先不下结论——下一步应当先确认测试自身的正确性。

### 诚实说明

- 测试用的 `python3.12` 与依赖库是从 Alpine 仓库下载后放在 `/tmp/alpine-root`，**不在仓库里**
- `tools/rune.c` 已在仓库中（`make tools/rune`），能复现上述所有结果
- 距离「Python 脚本跑通」还差若干指令，未达成

## 定位工具：单步历史环

调试「坏指针从哪来」时，把 fault 那条指令看了没用——需要的是**它前面几十条**。测试台因此维护一个 32 条的 `(pc, insn, x0, x1, x2)` 环形缓冲，停止时整体 dump（`E2_TRACE_HIST=1`）。

第一次用它看 Python：

```
pc=0x10161080  insn=0xd37cee80   x0=0xff0     lsl x0, x20, #4
pc=0x10161084  insn=0xf8606a60   x0=0x0       ldr x0, [x19, x0]
pc=0x10161088  insn=0xeb17001f   x0=0x0       cmp x0, x23
pc=0x1016108c  insn=0x54000420   x0=0x0       b.eq  -> 跳到解引用处
```

**第一次读这个 trace 时得出了错误结论**：它看起来像 `b.eq` 被错误地执行了。原因是 `ctx.instruction` 保存的是**上一步**的指令，与当前 `pc` 配对后就整体错位一格。改成直接从 `pc` 读指令字之后，链路才自洽：

```
0x1016107C  lsl  x0, x20, #4      x20 = 0xff -> 0xff0
0x10161080  ldr  x0, [x19, x0]    x19 = 0x104d0108 -> [0x104d10F8] = 0
0x10161084  cmp  x0, x23          0 vs 0x105c41c8
0x10161088  b.eq                  未跳转
0x1016108C  ldr  x1, [x0, #0x18]  解引用 NULL
```

`0x104d0108` 起的表是 256 项 × 16 字节，`0xff` 正是**最后一项**；该槽为 0，而不是代码比较的哨兵值 `0x105c41c8`。所以是「表里该有对象却没有」，不是解释器算错了指针。

顺带把 `lsl x0, x20, #4`（`UBFM` 在 `imms >= immr` 时的形式）在 0、3、全 1、2^60 四种输入下验证并加入常驻测试（现共 67 项）。

## 收尾状态（如实记录）

| 里程碑 | 状态 | 证据 |
| --- | --- | --- |
| M1 解释器跑 hello world | ✅ | 57 项基础检查 + guest 程序 |
| M2 syscall 层 + busybox 基础命令 | ✅ | `busybox ls` 列出 17 个目录，104,206 条指令，exit 0 |
| M3 sh 脚本 / 管道 / 重定向 | ✅ | 14 种 shell 形式全部正确，含 `ls \| wc -l` = 17 |
| M4 python 脚本 | ❌ **未达成** | Python 3.12 启动到 8,322,174 条指令后停住 |

**M4 的差距是明确的，也不小**：Python 已经能加载、重定位、跑过启动的大部分，但停在一处自己查找路径里的 0 值上。这既不是缺指令，也不是 `mmap`/`brk` 失败。继续缩小范围需要跨越数百万条指令的追踪窗口，超出单轮预算。

**其他未做的**：

- iOS 侧的薄封装需要 Xcode 工程改动，在 Linux 上无法验证，因此**没有做，也没有宣称做过**
- Python 二进制与依赖库不在仓库内，是测试时从 Alpine 仓库下载到 `/tmp/alpine-root` 的
- `grep`（不带 `-F`）仍在正则引擎内 fault，未解决

**CI**：`.github/workflows/e2.yml` 在干净的 Linux runner 上重建全部 guest 程序并跑 67 项检查。

## 关键修复：带寄存器偏移的有符号加载没有做符号扩展

追查 Python 的空指针时，逐层回溯到这张表：`x19` 起 256 项 × 16 字节，`x20 = 0xff` 被当作索引，取到的是最后一项（空槽）。

再往回一层，发现了真正的原因：

```asm
ldrsb x20, [x25, x21]      ; 读一个字节
```

字节是 `0xFF`，按有符号加载**应该**扩展成 `0xFFFFFFFFFFFFFFFF`（即 -1）。但历史记录里 `x20 = 0xff`。

查实现：**寄存器偏移**这条加载路径直接调 `write_reg`，**从未经过 `apply_load_opc`**，所以 `ldrsb`/`ldrsh`/`ldrsw` 全都不做符号扩展，原始字节被原样存下。

后果是系统性的：任何从字节表读出来当索引用的值，只要高位为 1 就会变成 255 而不是 -1，索引立刻落到表外。

修复后：

```
修复前  Python 停在 8,322,174 条指令，ldr x1, [x0,#0x18] 解引用 NULL
修复后  不再 fault —— 跑过 40,000,000 上限，再跑过 400,000,000 依然无错
```

### 下一步的问题：死循环还是慢？

测试台现在每五百万步打印一次 pc 与 syscall 计数：

```
5000000   pc=0x5006a594   syscalls=15
10000000  pc=0x101610d4   syscalls=53
15000000  pc=0x10161080   syscalls=53
...
60000000  pc=0x10161074   syscalls=53
```

**syscall 计数从 53 之后再没动过，pc 一直落在 `0x10161074`–`0x101610D4`。** 所以是死循环，不是慢。

历史 dump 给出循环体：

```asm
ldrsb x20, [x25, x21]      ; 探测表字节
tbnz  x20, #63, found      ; 负数 = 探测结束
lsl   x0, x20, #4
ldr   x0, [x19, x0]
...
add   x21, x21, x21, lsl #2
and   x21, x21, x27        ; 下一个探测下标
```

这是一个**哈希探测链**：沿链查找直到遇到符号位为 1 的字节。它永远遇不到，于是下标无限循环。

**读到寄存器后，答案很清楚：**

```
x25 = 0x1065c050   探测表 8 项:  ff 00 00 00 00 00 00 00
x19 = 0x1065c058   值表:         0:0x105c41c8，其余全 0
x27 = 7            掩码
```

槽 0 的 `0xff` 符号扩展后是 -1，**正是终止探测的条件**——表里有终止标记。

但 trace 里下标依次是 `5, 5, 1, 2, 6, 2, 6, 1, 3`，**从未到过 0**。步进公式是 `(5 * x21 + x26 + 1) & 7`，这个序列覆盖不到全部 8 个槽，于是绕过唯一能终止它的那个槽。

所以剩下的问题很窄：是 `x26` 算错了导致步进序列错过 0，还是这张表本就不该在这个掩码/装载因子下探测。两者都能从同一份 trace 回答。

新增三条常驻检查（-1、127、-128 三种字节经 `ldrsb`），共 **70 项**。

## 收尾：轮次结束时的真实状态

目标要求四个里程碑。到本轮为止：

| 里程碑 | 状态 | 可复核的证据 |
| --- | --- | --- |
| M1 解释器跑 hello world | **完成** | 70 项主机检查；`hello.elf`/`fib.elf`/`cprog.elf` 等 guest 程序 |
| M2 syscall 层 + busybox 基础命令 | **完成** | `busybox ls` 列出 17 个目录、104,206 条指令、exit 0 |
| M3 sh 脚本 / 管道 / 重定向 | **完成** | 14 种 shell 形式全部正确，含 `ls \| wc -l` = 17 |
| M4 python 脚本 | **未完成** | Python 3.12 启动后死循环在哈希探测里 |

### M4 差距的具体位置

不是缺指令，不是崩溃，也不是内存耗尽——是**一个探测循环绕过了自己的终止标记**：

- 探测表 8 项，槽 0 的 `0xff` 就是终止标记（符号扩展后 -1）
- 下标序列 `5, 5, 1, 2, 6, 2, 6, 1, 3` 覆盖不到 0
- 步进是 `(5 * x21 + x26 + 1) & 7`

下一步只需回答一件事：`x26` 是不是算错了，导致步进序列错过槽 0。

### 本轮之前修掉的关键缺陷

M4 能推进到这里，靠的是最后几轮修掉的一个系统性错误：**带寄存器偏移的有符号加载不做符号扩展**（`ldrsb`/`ldrsh`/`ldrsw`）。一个 `0xFF` 字节被当成 255 而不是 -1，任何用它做索引的地方都会落到表外。修复前 Python 在 8,322,174 条指令处解引用 NULL；修复后不再崩溃。

### 明确未做

- **iOS 薄封装**：需要 Xcode 工程改动，在 Linux 上无法验证，**没做也没宣称做过**
- **`grep` 正则引擎**：不带 `-F` 时仍在 `0x4c4408` fault
- Python 二进制与依赖库不在仓库内，是测试时从 Alpine 仓库下载到 `/tmp/alpine-root` 的

### 可复核性

```bash
make -C e2 test           # 70 项，干净环境会先从源码重建全部 guest 程序
make -C e2 overlaps       # 解码规则重叠检查，musl 与 busybox 均无重叠
make -C e2 tools/rune     # 主机测试台
.github/workflows/e2.yml  # 同一套检查在 CI 上跑
```

## M4 进展与当前阻塞点

Python 3.12.13 能启动并打印版本号（`-V`）。执行代码仍失败，但每次失败都是
**具体的、可复现的** fault，不再是死循环。已修掉的问题按因果顺序：

1. **UMOV 被当成 DUP 解码** —— `memset` 的 8 字节模式全写成 0。
2. **UMOV 掩码过宽** —— 吞下 `cmhs`/`cmge`/`fdiv`。
3. **LD1 lane 掩码过宽** —— 吞下 `ld1r`。
4. **FMAX/FMIN 当成 *NM 变体** —— NaN 传播语义错。
5. **符号扩展只到 32 位** —— `ldrsb x2,[x2,#32]` 返回 0xFFFFFFFF 而非全 1，
   使 Python 哈希探测永不终止。这解掉了死循环。
6. **MAP_FIXED 未实现替换语义** —— musl 用匿名 `MAP_FIXED` 覆盖各段的 `.bss`，
   而实现只是「接受地址」，导致 `.bss` 保留文件字节（`0x459930`，
   `.data.rel.ro` 的 vaddr），被当作线程状态指针解引用。
7. **FPCR / FPSR 未实现** —— musl 的 fenv 代码会读改写 FPCR。
8. **INS（通用寄存器→向量）未实现** —— UMOV 的逆操作。

当前停在这里：`pc=0x1018abd8`，`ldr x2, [x0, #0x90]` 且 `x0 = 0` —— 一个空指针。
指令数从 11,587,837 推进到 12,848,935。

### 调试工具

- `E2_TRACE_STACK=1` —— 影子调用栈（bl/blr 入栈、ret 出栈，48 层），fault 时打印。
- `E2_TRACE_WATCH=1` —— 逐指令比对某个 guest 字，变化时报出执行的那条指令。
- `E2_TRACE_CODE=1` —— fault 时 dump 出错位置附近的真实代码。
  磁盘文件和内存映像的偏移不一定一致，反汇编文件曾导致错误结论。
- `tools/check-rule-mnemonics.py` —— 把解码规则的分类与 llvm 的独立反汇编做全量比对。
  它一次找出了上面第 2、3 条。

### 当前阻塞点（持续更新）

`python3 -c pass` 停在 `pc=0x10192160`：一条内联引用计数自增

```
ldr w0, [x1]     <- x1 = 0
adds w0, w0, #1
str w0, [x1]
```

它不是解码问题。已确认的事实：

- 调用点 `0x1017c568` 传的就是 `x1 = 0`，来源是 `ldr x1, [x22, #0x18]`。
- `x22 = 0x10806020` 是有效指针，八个字段里五个是有效指针。
- `+0x18` 全程从未被写入（逐指令监视整整 1285 万条指令无变化）——
  不是写了又清空，而是分配出来就是空的。

所以这是 Python 自身的记账数据不对，不是内存或解码错误。下一步应从「这个结构体
是什么对象、谁该填 `+0x18`」入手，而不是继续沿指令流走。

### 已确认正常的行为

- musl 用一次整文件 `mmap` 装载 libpython（`len` 等于文件大小）——qemu 下同样如此。
- 段的 `.bss` 由随后一个匿名 `MAP_FIXED` 覆盖清零；这条之前被忽略，是坏指针的根因。

### 与 qemu 逐点对照（有效手段）

定位「值被当作指针」这类问题时，最有效的是**在同一代码位置对照 qemu**：

```sh
# 起 qemu 的 gdb stub（libpython 基址 0x4000008e5000，取自 -strace 的 mmap 返回值）
qemu-aarch64-static -g PORT -L /tmp/alpine-root /tmp/alpine-root/usr/bin/python3.12 -c pass
# 目标地址 = 基址 + (guest_pc - 0x10000000)
```

用最小 GDB RSP 客户端设断点、读寄存器与内存即可。`tools/` 里没有收录这个脚本，
它是临时写的；需要时按 RSP 的 `Z0`/`c`/`g`/`m` 四个命令实现即可。

它直接给出了答案：同一个结构体在 qemu 下 `+0x18`/`+0x20`/`+0x28` 三个字段是同一个
指针，而本实现里前两个为空——于是问题被锁定到「写这两个字段的存储指令」，而不是控制流。

**局限**：只能验证「这个位置在 qemu 下是什么」，不能直接找到「第一处分歧」。
如果 qemu 根本不执行那条指令，说明分歧更早，需要别的手段。

### 当前阻塞点的真实性质（重要）

`python3 -c pass` 在 1.5 亿条指令后 fault，但**fault 不是根因**。stderr 实际输出是：

```
Exception ignored error evaluating path:
Traceback (most recent call last):
```

也就是说：**Python 在计算 `sys.path`（执行冻结的 `getpath.py`）时抛出了异常**，
随后在打印回溯、取源码行时踩到坏指针而崩溃。

关键推论：**qemu 下那段代码从不执行**（用硬件断点验证过），因为正常运行时没有这个
异常。所以这不是解码问题，也不是内存问题，而是**某个 Python 操作失败**。

下一次排查应该从这里入手，而不是继续修指令：

1. 让回溯能正常打印，从而看到真正的异常类型与消息；
2. 或者直接定位 `getpath.py` 里哪一步失败——它依赖 `os`、`sys`、文件系统调用；
3. `-S`、`-E`、`-X frozen_modules=off`、`PYTHONHOME` 都已试过，都绕不过去。

### 测试台的环境变量

`tools/rune` 现在会把 `PYTHONHOME`、`PYTHONPATH`、`PYTHONNOUSERSITE` 透传给 guest，
方便在不重编译的情况下做这类实验。

### 已补齐的路径类系统调用

排查 getpath 失败时发现两个缺失项，都是任何程序都会用到的：

| 调用 | 之前 | 现在 |
| --- | --- | --- |
| `getcwd` (17) | ENOSYS | 返回 guest 的 cwd（`e2_syscall_env.cwd`） |
| `chdir` (49) | 未实现 | 更新 cwd |
| `readlinkat` (78) | ENOSYS | 经 `e2_file_ops.readlink` 交给宿主 |

补齐后 Python 的错误输出**没有变化**，说明 getpath 的失败不是系统调用引起的，而是纯
Python 层的计算问题。所有系统调用现在都返回正常（`readlinkat` 对非符号链接返回
EINVAL 是正确行为）。

### 报错点的确切位置

字符串 `"error evaluating path"` 在 libpython 的 file offset `0x304a58`（vaddr 同值），
代码里只有一处引用：`adrp` 在 vaddr `0x203524`，紧接着 `add x19, x0, #2648`，随后
`mov x1, #0` 并调用 ——即 `_PyErr_WriteUnraisableMsg("error evaluating path", NULL)`。

下一次可以从这里反推：调用它的分支条件是什么（前面是 `ldr x0,[x20]` / `sub` / `str` /
`cbz` 的引用计数递减），以及 `x20` 持有的异常对象是什么。

### 已查明待处理异常（重要进展）

通过 TPIDR_EL0 首字找到真实的 thread state（注意：**首字就是 tstate 指针**，
不是 `TPIDR + 固定偏移`，这一点走了弯路），`+0x60` 即 `curexc_value`：

```
ValueError: embedded null character
```

23 个字符，与 `PyASCIIObject` 的 length 字段吻合。读取要点：

| 偏移 | 含义 |
| --- | --- |
| obj+0x08 | 类型对象 |
| obj+0x18 | `args` 元组（`dict` 在 +0x10，别搞混） |
| tuple+0x10 | 元素个数 |
| tuple+0x18 | 元素 0 |
| str+0x10 | 长度 |
| str+0x28 | **紧凑 ASCII 的字符起点**（+0x20 是 state 字，不是数据） |

所以 `getpath` 阶段有个**路径字符串里混进了 NUL 字节**。下一步应查：
`sys.executable`、`sys.prefix`、环境变量、以及 argv[0] 是如何被拼成路径的。

### 调试工具

`E2_TRACE_TSTATE=1` 在 `_PyErr_WriteUnraisableMsg` 处 dump thread state，
自动解出类型名与异常消息。这是目前最有用的探针。

### M2 的实际覆盖度（可复现）

M2 的结论不能靠回忆，所以有了 `tools/m2-sweep.sh`：它跑 47 个 busybox applet，
报告哪些活下来。

```sh
e2/tools/m2-sweep.sh /path/to/busybox [e2/tools/rune]
```

注意每个 applet 的 stdin 重定向自 `/dev/null` —— `sort`、`tr`、`wc` 会读 stdin，
否则会把命令列表剩下的部分吃掉（第一次跑就踩了这个，只测了 23 条）。

用一次扫描代替「应该没问题」的判断，直接带来一个修复：`seq` 在 7,454 条指令处
停在未实现的指令上。顺着它补了五组指令：

| 指令 | 说明 |
| --- | --- |
| `fmov d9, d8` | 浮点一源组原先没有通用处理器，只有 FRINT 用表匹配 |
| `fmov v0.d[1], x3` / `fmov x3, v0.d[1]` | 128 位寄存器上半部分有独立的编码对 |
| `adc` / `adcs` / `sbc` / `sbcs` | 与普通移位加减不同组：bits 28-24 = 11010 |
| `fmadd` / `fmsub` / `fnmadd` / `fnmsub` | bit 21 控制减加数，bit 15 控制负乘积，且**融合**（乘积不先舍入） |

`seq 3`、`seq 1 5`、`seq 10 -2 1` 现在都正确，最后一条还验证了负步长。

### 已知但未修：`grep` 正则 — 已定位到第一处分歧

`grep -F` 正常，任何正则模式都 fault，与模式无关。输入读取正常，
崩溃在正则编译阶段。

**用单步轨迹对比定位到了确切分歧点。** 方法：busybox.static 是 ET_EXEC、
固定 `0x400000`，地址在 qemu 下完全相同。从函数入口 `0x4c6a38` 开始，
qemu 侧用 gdb stub 单步 400 次采轨迹，本实现用 `E2_TRACE_PCS=0x4c6a38`
采同一段，然后逐条比对。

```
第一处分歧：索引 236
  [235] qemu=4b8458  mine=4b8458
  [236] qemu=4b845c  mine=4b852c   <<<
```

分歧指令：

```asm
0x4b8450: ldr  x0, [x19, #8]
0x4b8454: cmp  x19, x0
0x4b8458: b.eq 0x4b852c
```

两边的值完全不同：

| | x19 | [x19] | [x19+8] |
| --- | --- | --- | --- |
| qemu | `0x524180` | `0x524158` | `0x524158` |
| 本实现 | `0x20001108` | `0x20001108` | `0x20001108` |

堆地址不同是正常的（各自的 brk 位置不同）。**关键是结构语义不同**：
qemu 下该节点指向另一个节点（链表 ≥2 项），本实现下三个值全等于自身
（空的自环链表）。

所以问题不是控制流，而是**某个链表插入没有发生** —— 一次静默的内存写错，
PC 轨迹到这一点为止完全相同。下一步要查的是：谁本该把那个节点挂进去。

### 方法：单步轨迹对比

这是目前最有效的定位手段，值得记下做法：

1. 确认目标二进制是 ET_EXEC（地址固定），PIE 不行；
2. qemu 侧：`-g PORT` 起 gdb stub，断在函数入口，连续 `s` 单步并记录 pc；
3. 本实现侧：`E2_TRACE_PCS=0x地址 ./tools/rune ...` 采集同一段；
4. 逐条 diff，第一个不同的下标就是分歧点。

坑：qemu 的 gdb stub 不跟随 fork 出的子进程，所以必须直接调用 applet，
不能经 `sh -c '... | grep'`。

#### 继续追踪：链表头被插入后又被清空

用本实现的写陷阱（qemu 的 `Z2` 观察点不被支持，返回空）监视链表头 `0x520460`，
整个运行只被写两次：

```
pc=0x4b8208  val=0x20001108   ← 插入节点，头指向它
pc=0x4b852c  val=0x0          ← 清空
```

而 `0x4b852c` **正是 0x4b8458 那条 `b.eq` 的跳转目标**。

因果链于是很清楚：

1. `0x4b8208` 把一个节点挂上链表，头 = 该节点
2. 该节点的 `next`/`prev` 是**自环**（空环的形态）
3. 到 `0x4b8458`：`x19` = 头，`x0` = `[x19+8]` = 节点自身 → `cmp` 相等 → 跳转
4. 跳到 `0x4b852c` 把头部**清成 0**，链表丢失

在 qemu 下同一时刻头节点的 `next`/`prev` 指向**另一个节点**（`0x524158`），
`cmp` 不相等，所以正常往下走。

反汇编 `0x4b8208` 之后这一步的判断又要修正一次：

```asm
0x4b8204: stp x19, x19, [x19]    ; node->next = node; node->prev = node
0x4b8208: str x19, [x20, #16]    ; head = node
```

这是**创建一个空环形链表**的标准写法，自环是正确的初始形态，不是写错。

所以先前「next/prev 写成了自环所以是 bug」的说法不成立。真正的情形是：
qemu 下到 `0x4b8458` 时链表里已经有 **两个**节点（头节点 + `0x524158`），
即 `0x4b8208` 之后还有一次插入；而那次插入**不改变头指针**，所以只监视
`0x520460` 看不到它。

**下一步**：在 `0x4b8208` 到 `0x4b8458` 之间找那条把第二个节点挂进链表的代码，
（链表插入通常是 `node->next = head->next; head->next->prev = node; ...`）
确认它在本实现里是否真的改到了两个节点的链接字段。

#### 分歧点又向前追了一层

用同样的单步对比，从最外层帧 `0x4b69f0` 采集 900 条轨迹：

```
前 201 条完全一致
第一处分歧：索引 304
  [303] qemu=4d0bd4  mine=4d0bd4
  [304] qemu=4d0bd8  mine=4d0bac   <<<
```

分歧指令：

```asm
0x4d0bb0: ldrb w4, [x1, x2]
0x4d0bb4: cmp  w4, #0
0x4d0bb8: ccmp w3, w4, #0, ne
0x4d0bbc: cbz  w3, 0x4d0bd8
0x4d0bc0: cset w6, eq
0x4d0bc4: cmp  x7, x2
0x4d0bc8: cset w5, ne
0x4d0bcc: add  x2, x2, #1
0x4d0bd0: tst  w6, w5
0x4d0bd4: b.ne 0x4d0bac        ; qemu 不跳，本实现跳
```

一个字节比较循环。两侧的 `x1` 都指向栈上的字符串数组，`x6` 都是 0。

**已排除**：环境变量内容。用 `env -i` 给 qemu 传入与本实现相同的 5 个环境变量，
qemu 仍正常输出、本实现仍 fault，所以不是 `envp` 内容差异造成的。

**矛盾已解决**：把寄存器 dump 嵌进轨迹记录内部（`E2_TRACE_AT2`），保证与轨迹下标
是同一迭代。索引 303 的读数是 `x6 = 1`，所以 `tst w6, w5` 确实应当跳转——之前
那个单独的探针命中的是别的迭代，读数无效。

判定到具体的字节差异：

| | x3 | x4 | x6 |
| --- | --- | --- | --- |
| qemu | `0x4c` 'L' | **`0x53` 'S'** | 0 |
| 本实现 | `0x4c` 'L' | **`0x4c` 'L'** | 1 |

`x4` 来自 `ldrb w4, [x1, x2]`。dump 出本实现这一侧的字符串是：

```
"LANG=C"
```

也就是**本实现在扫描环境变量里的一条字符串**，而 qemu 那边扫的是另一条以 `'S'`
开头的。这个循环把 `w3` 与字符串逐字节比较：本实现第一个字节就匹配（都是 'L'）
所以继续走，qemu 第一个字节不匹配所以停下。

**「扫的是环境变量」这个判断是错的。** 加 `E2_MIN_ENV=1` 完全不给环境变量后，
仍然 fault（只是指令数变成 17132）；再单独去掉 `LANG` 也仍然 fault。

所以 `x1` 指向 `"LANG=C"` 只是**栈地址巧合**——它落在环境字符串区旁边，
而那段代码读的是栈上任意内容，不是 `environ`。`x1` 在本实现里指向的是
未初始化或被复用的栈内存。

**已排除**：locale（LANG）、envp 内容、envp 数量、argv[0]、cwd。
**当前判断**：这个字符串比较函数拿到的是**错误的指针**，而指针从哪来
还没查到——它的调用者在当前反汇编里找不到入口（可能是尾调用或更大函数的一段）。

#### 影响面：三个核心文本工具同一个根因

扩展 M3 扫描（43 个构造）时又发现两个失败，它们的故障位置与 `grep` **完全相同**：

| 命令 | 故障 pc | 故障地址 |
| --- | --- | --- |
| `grep a file` | `0x4c4408` | `0x10006018` |
| `sed s/a/A/ file` | `0x4c4408` | `0x10006028` |

同一台机器、同一条指令——都是 BRE 正则引擎。

所以这一个 bug 影响 **`grep`、`sed`、`awk` 三个核心文本工具**，
而不只是 grep。修它的价值比最初估计的高得多。

（`awk '{print NF}'` 在直接调用形式下没有复现，可能是参数引用方式的问题，
未进一步确认。）

### awk：同样的 NULL 指针症状

批量测试 applet 时发现 `awk` 也坏，但只在特定路径：

| 调用 | 结果 |
| --- | --- |
| `awk '{print}' file` | 正常 |
| `awk 'BEGIN{print 1}'` | fault |
| `awk 'END{print 1}' file` | fault |
| `awk 'BEGIN{x=1}'` | 正常 |

也就是**没有输入上下文时的 `print`** 会崩。

定位到 musl 的 `fwrite`：`mul x21,x1,x2` 算总字节、`ldr w1,[x3,#140]` 读 FILE 标志，
而 **`x3`（FILE*）是 NULL**。

qemu 下同一位置 `x3 = 0x520200`——musl 的静态 `__stdout_FILE`。

**排除了加载器**：那个指针的静态初值就在文件里（vaddr `0x51f480`），
本实现读出来的正是 `0x520200`，说明 .data 段加载正确。NULL 是运行时产生的。

**与 grep 的关系**：两者都是「该有有效指针的地方拿到 NULL/自环」，
很可能同一个根因，但都还没找到。

顺带记一个设施缺口：`e2_apply_relative_relocations` 在 `src/loader.c` 里存在，
**但没有任何调用者**。对静态 ET_EXEC 目前不需要它（初值就在文件里），
但静态 PIE（ET_DYN）会需要——现在的 loader 只走 tests。

#### awk 的 NULL FILE*：又排除了四项

顺着 `awk 'BEGIN{print 1}'` 往下查，这一轮的否定结果（都值得记，免得后面重做）：

| 假设 | 结论 |
| --- | --- |
| 加载器漏了 .data | **不成立**：`0x51f480` 读出来正是 `0x520200` |
| 静态重定位没做 | 不需要：静态 ET_EXEC 的初值就在文件里 |
| FILE 对象没初始化 | **不成立**：`0x520200` 处 `+0=0x5`、`+24=0x4caa68`（write 函数），是有效的 FILE |
| 缺系统调用 | **不成立**：全部返回成功，仅 `newfstatat` 返回 -2（正常的存在性检查）|

写陷阱显示，整个运行只有两次写入覆盖数据段，都来自加载/`execve`
（`0x4c2ba8` 是 execve 的 `svc #0`）——即 shell exec awk 的正常重载。

所以 NULL 是 awk 自己算出来的，而它读的**不是** musl 的 `stdout` 变量
（那个位置的值是对的）。下一步应找出 awk 内部那个为 NULL 的指针变量是哪个。

**顺带**：`munmap` 目前是空实现（只返回 0），内存永不释放。这不是损坏源，
但会让反复分配/释放的程序（grep、awk 这类）地址空间只增不减。

#### 确认：awk 和 grep 是同一个 bug

用单步轨迹对比跑 `awk 'BEGIN{print 1}'`，从 `0x4b69f0` 采 700 条：

```
前 303 条完全一致
第一处分歧：索引 304
  [303] qemu=4d0bd4  mine=4d0bd4
  [304] qemu=4d0bd8  mine=4d0bac
```

**和 grep 的分歧点一模一样**——同一地址 `0x4d0bd4`、同一索引 304。
所以这不是两个 bug，是一个。

分歧所在的循环是**字符查找**（`strchr` 类）：扫描字符串找一个字节。

```asm
0x4d0bb0: ldrb w4, [x1, x2]     ; 取一个字节
0x4d0bb4: cmp  w4, #0
0x4d0bb8: ccmp w3, w4, #0, ne   ; 与目标字符比较
0x4d0bbc: cbz  w3, 0x4d0bd8     ; 找到就退出
0x4d0bc0: cset w6, eq
0x4d0bc4: cmp  x7, x2
0x4d0bc8: cset w5, ne
0x4d0bcc: add  x2, x2, #1
0x4d0bd0: tst  w6, w5
0x4d0bd4: b.ne 0x4d0bac         ; 没找到且未到末尾就继续
```

两边读到的字符串：

| | x1 | 内容 |
| --- | --- | --- |
| qemu | `0x4000008004d0` | `SHELL=/bin/bash…` |
| 本实现 | `0x7f0ff820` | `LANG=C` |

都是栈上的环境字符串，但**是不同的那一条**。所以问题在**遍历的起点或索引不同**，
而不是某条环境变量本身——这一点有支撑：用 `env -i` 给 qemu 传入与本实现相同的
环境变量后，qemu 仍然正常、本实现仍然 fault。

**下一步**：确认这个字符查找被调用时，`x1` 是由哪个数组项算出来的。两边遍历的
不是同一个数组（或同一项），这是唯一还没解释的差异。

#### 澄清：`0x4d0bd4` 是 locale，不是那个 bug

上一轮把 `0x4d0bd4` 当作 grep/awk 的共同根因。**这个判断错了。**

那一处是 `setlocale` 在环境变量里找 `LC_ALL`/`LANG`：`x0` 指向静态名表
`"LC_ALL\0LANG\0POSIX\0MUSL_LOCPATH\0"`（两侧相同），`x1` 指向一条环境变量。

| | x1 的内容 |
| --- | --- |
| qemu | `SHELL=/bin/bash`（首字母 S，与 `LC_ALL` 的 L 不同，立即退出）|
| 本实现 | `LANG=C`（首字母 L 相同，继续比较下去）|

**两边各自的行为都是对的**，只是环境内容不同。

**决定性证据**：用完全空的环境跑，这个循环**根本不会被进入**（探针 0 次命中），
而 grep **照样 fault**，位置还是 `0x4c4408`。

所以真正的 bug 是**与环境无关**的那一个，一直就在 `0x4c4408`。

#### 新工具：qemu 的 `-strace`

```sh
env -i qemu-aarch64-static -strace busybox grep a file
```

对比下来，mmap 序列完全相同（4096、4096、16384），所以分配模式不是差异源。

唯一的结构性差别是 **`brk` 起点**：qemu 是 `0x523000`（紧跟镜像之后），
本实现硬编码 `0x20000000`。试过让 loader 把 `brk_base` 设成镜像末尾——
**没能修好 grep**（故障点不变），所以已回滚。

（回滚过程中踩到一个坑，值得记：`brk` 的映射判断原本用 `brk_base == 0`，
loader 一旦设置了这个字段，映射就被跳过，`echo hi` 都会挂。正确做法是用
`brk_limit` 作为「已映射」标志。改动虽已回滚，但这条经验留着。）

#### 轨迹对比工具的三处修正（前两处都是我的 bug）

**1. 索引对齐是错的。** qemu 的 gdb stub 在 `svc` 处不保证逐条上报，
按索引逐条对比会在**每个系统调用处**报假分歧。

**2. 缓冲区满后每条指令重复打印整个缓冲。** 原来写的是「打印后置
`recording = -1`」，但打印条件 `recording && n == 512` 在置位之前判断，
填满之后每一步都会重印一遍。结果是：**第 512 条之后的轨迹全是缓冲重复的垃圾**。

这意味着**之前所有超过 512 条的轨迹对比结论都无效**（≤512 的仍然有效，
所以索引 236、304 那两次没问题）。修复：8192 缓冲 + 打印前先停记录 +
`E2_TRACE_PCS_LIMIT` 指定采样长度。

**3. 工具已固化**：`tools/trace-align.py`。

```sh
tools/trace-align.py qemu.txt mine.txt
```

#### 工具修好之后：真正的位置在 `0x4c7fcc`

用修好的工具从 `0x4a0060` 重采，前 600 条完全一致；采到 2000 条时：

```
qemu[1607]=4c7fd0   mine[1608]=4c6f1c
qemu: 4c7fbc 4c7fc0 4c7fc4 4c7fc8 4c7fcc 4c7fd0 …
mine: 4c7fbc 4c7fc0 4c7fc4 4c7fc8 4c7fcc 4c6f1c …
```

分歧指令：

```asm
0x4c7fc4: ldr  w1, [x24, #20]
0x4c7fc8: tbz  w1, #31, 0x4c6f1c     ← 目标正是本实现走的地址
```

| | w1 的 bit31 | 行为 |
| --- | --- | --- |
| qemu | 1 | 不跳 |
| 本实现 | 0 | 跳转 |

`[x24+20]` 由同一函数里的 `stp w20, w1, [x24, #20]` 写入，
而 `w20` 来自 `ldr w20, [x2, x0]` 的一次表查找。**又是值不同，不是控制流错。**

（前几轮基于「索引 377」「索引 236 之后的 608 条差距」的推理，
凡超过 512 条的都建立在上面的工具 bug 之上，已作废。）

### 找到了两个根因：MVNI 的 op 位 与 DCZID_EL0

顺着 `0x4c7fc8` 的 `tbz w1,#31` 追下去，写陷阱显示那个字段被 `str d0,[x0,#16]`
写成 **0**，而 `d0` 来自上一条 `mvni v0.2s, #0`——本该是全部置 1。

**根因一：`mvni` 忽略了 bit 29。** MOVI 和 MVNI 共用编码，bit 29 区分两者，
但实现里只对字节通道（cmode 0xE）用了这个位，其余 cmode 把 `mvni` 当 `movi` 执行。
正则引擎用这个模式在新建状态里写「未设置」标记；标记永远写不进去，
于是每个状态都像是已设置，匹配恒失败。

**根因二：`DCZID_EL0` 的值写反了。** 原来返回 `0x4`（DZP=0，表示**允许** `dc zva`），
而注释写的是「no DZP」。musl 的 `memset` 会查这个值并据此走 `dc zva` 快路径。
现在返回 `0x10`（DZP=1）——对一个 DC ZVA 不该被 C 库依赖的核心，这是诚实的答案。

两条都是**「声称某个能力可用，实际并没有」**这一类：第一次是掩码声称覆盖了
op 位，第二次是寄存器声称 DC ZVA 可用。症状各不相同，成因是同一个。

修完之后：

| | 之前 | 现在 |
| --- | --- | --- |
| `grep a file` | fault（`0x10006018`）| 跑完，仍不匹配 |
| `sed s/a/X/` | fault | 跑完，仍不替换 |
| M2 扫描 | 63 通过 / 1 已知 | **64 通过 / 0 失败** |
| M3 扫描 | 57 + 2 已知 | **59 通过 / 0 失败** |

两个扫描都不再有已知失败。但**正则匹配仍然错误**——还有至少一处分歧，
正在用轨迹对比继续追。

### 本轮修好的三个根因（以及仍未解决的部分）

顺着 grep 的崩溃往下追，找到三个**「声称某个能力可用、实际没有」**的缺陷：

| # | 缺陷 | 症状 |
| --- | --- | --- |
| 1 | `mvni` 忽略 bit 29 | 模式全 1 变成 0，正则引擎的「未设置」标记永远写不进去 |
| 2 | `DCZID_EL0` 返回 4（DZP=0）| memset 走 `dc zva` 快路径却什么都没写 |
| 3 | `cmeq`（向量）未实现 | 字符集比较没有分支，直接 unimplemented |

修完之后 **grep 和 sed 不再崩溃**，M2 扫描 64/64、M3 扫描 59/59，两个扫描
第一次都不再有已知失败。

### 仍未解决：非空模式匹配不上

现在的状态很明确：

| 命令 | 结果 |
| --- | --- |
| `grep '' file` | 正常（匹配所有行）|
| `grep 'a' file` | 退出码 1，无匹配 |
| `grep '^'`、`grep '$'` | 同上 |
| `grep -F a file` | 正常 |
| `grep '[' file` | **正确报错** `bad regex '[': Missing ']'` |

最后一条很关键：**regcomp 是好的**（能编译、能检测语法错误），
问题在 **regexec**（匹配阶段）。

已经排除的定位手段与结果：

- 从 ELF 入口对比轨迹：分歧在 auxv 遍历，是**良性的**（两边 auxv 条目数不同）
- 从 `0x4a0060` 采 4000 条：**完全一致**
- 从匹配路径内部 `0x4b8458` 采 2500 条：**完全一致**

所以分歧在更深处。`0x4c4408`（最初的崩溃点）已经不在路径上。

**工具侧的两条重要修正**（都是我的 bug，之前几轮的结论受其影响）：

1. 索引对齐会在每个 `svc` 处报假分歧 → 改用顺序游走 + 容忍单条记录差异
2. 轨迹缓冲区满后**每条指令重印整个缓冲**，512 条之后全是垃圾
   → 8192 缓冲 + 打印前先停记录 + `E2_TRACE_PCS_LIMIT`

新增工具：`tools/trace-align.py`、`E2_TRACE_HIT`（判断某地址是否在路径上）、
`E2_TRACE_PCS_LIMIT`。

### 正则匹配：已确认为唯一剩余缺陷，且范围不断收窄

经过这一轮，**除正则匹配外的一切都被排除了**：

| 检查项 | 结果 |
| --- | --- |
| argc / argv 传递 | 正确（`busybox.static, expr, abc, :, abc`）|
| `strlen` | 正确（`expr length abc` = 3，两边一致）|
| 字符串子串 | 正确（`expr substr abcde 2 3` = bcd，两边一致）|
| `regcomp` | 正确（`grep '['` 正确报 `Missing ']'`）|
| `grep -F` | 正确 |
| 空模式 | 正确（匹配所有行）|
| **`regexec`** | **所有非空模式都返回不匹配** |

## 工具：qemu 的日志比 gdb 单步快几个数量级

这是本轮最有价值的产出。`gdb` 单步大约**每秒 100 条指令**，一条 3 万条指令的
轨迹要跑一小时，这让对比法只能用在很窄的窗口上。

```sh
env -i qemu-aarch64-static -d in_asm,exec -D log BINARY ARGS
tools/qemu-fultrace.py log > qemu.txt     # 逐指令轨迹，几秒钟
tools/qemu-execdiff.py qemu.txt mine.txt  # 找第一条不出现的块
```

实测：`expr a : a` 的 9,670 条指令轨迹**约两秒**生成。

## 三个会制造假分歧的坑（都踩过）

1. **索引对齐**：`svc` 处 gdb 不保证逐条上报，每个系统调用都报假分歧
2. **缓冲区满后重印**：512 条之后全是缓冲重复的垃圾
3. **argv[0] 形状**：busybox 会扫它找 `/`，`busybox.static` 和
   `/tmp/bbs/bin/busybox.static` 走不同分支——用 `E2_ARGV0` 对齐
4. **字符串对齐**：本实现原来把 argv/env 字符串补齐到 16 字节，而真实内核紧密
   排列；对齐的地址会让 musl 的 word-at-a-time `strlen` 走不同分支。已修正为紧密排列

### exec 日志的致命限制：看不见块内循环

`-d in_asm,exec` 的方案有个根本缺陷，花了很长时间才看清：

**qemu 的基本块内部循环不产生新的块入口。** `b.hi` 跳回**块内**时，
qemu 在翻译好的块里直接循环，不会记一条新的 `Trace`。所以：

| | 记录方式 |
| --- | --- |
| 本实现的 `E2_TRACE_PCS` | **每一条指令**一条 |
| qemu 的 `-d exec` | 每个**块入口**一条，块内循环合并 |

于是 musl 的 memset 循环看起来只跑了 2 次，实际跑了 4 次——
**这不是分歧，是记录方式不同**。

验证方式：`-d cpu` 能给出每个块入口的寄存器快照，

```
Trace: [../0000000000400538/..]  X02=00000000000000a0   ← 第 1 次
Trace: [../0000000000400538/..]  X02=0000000000000060   ← 第 2 次
```

只有两条记录，但 `X02` 从 0xa0 降到 0x60，按 `subs #64` 的进位逻辑本该还有第三次。
**块内循环被合并了**，这就是原因。

**结论**：exec 日志适合快速定位「走了哪条路」，**不适合数循环次数**。
涉及循环的对比必须用 gdb 单步，或者先确认分歧点不在循环里。

