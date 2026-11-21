# E2 解释器：CPython 3.12 启动崩溃 — 给 Codex 的问题报告

## 待分析的问题

自写的 aarch64 解释器（`e2/`，纯 C11，逐条解释 ELF）跑 Alpine 的 CPython 3.12。
**同一个二进制在 qemu-aarch64-static 下完全正常**，在解释器下执行 Python 代码时崩溃。

## 先看结论（已确定的部分）

```
FAULT reason=load outside mapped memory
addr=0xfffffff   pc=0x102fbbf4   insn=0x39c00024  (ldrsb w4, [x1])
x1=0xfffffff  x5=0x7fffe970  x6=0x10000013  x19=0x7fffe970  sp=0x7fffe940
```

**扫描循环本身是对的**，坏的是它拿到的指针：

```asm
; PyCode_Addr2Line —— 0x2800f0
2800f4: mov  x3, x0            ; x3 = code object
28011c: ldr  x3, [x3, #0x88]   ; x3 = co_linetable      ← 这里拿到的不是堆指针
280128: mov  x1, sp            ; 第二个参数是调用者的局部结构
28012c: add  x4, x3, #0x20     ; 数据起点
280130: ldr  x3, [x3, #0x10]   ; 长度（ob_size）
28013c: add  x3, x4, x3        ; 数据终点 = 0x10000013
280148: bl   _PyCode_CheckLineNumber

; 0x2fbbb0（静态函数）—— 反向扫描
2fbbf0: sub   x1, x6, #2
2fbbf4: ldrsb w4, [x1]         ← 崩在这里
2fbbf8: sub   x1, x1, #1
2fbbfc: ldrb  w2, [x1, #1]
2fbc00: tbz   w4, #31, 0x2fbbf4 ; bit31 为 0 就继续（扫过 ASCII），遇到 0x80+ 才停
```

`0x10000013` 落在 **libpython 自己的映射里**（基址 `0x10000000`），
那里是 ELF 头的零字节 —— bit31 全是 0，于是一路向后走过映射边界，崩在 `0xfffffff`。

**所以：`co_linetable` 这个字段里存的是 libpython 基址附近的值，而不是堆上 bytes 对象的指针。**

## 已排除的方向

| 尝试 | 结果 |
| --- | --- |
| `-S`（跳过 site）| 同样崩 |
| `-E`（忽略环境）| 同样崩 |
| `-I`（隔离模式）| 同样崩 |
| `-P`（不把 cwd 加进 sys.path）| 同样崩 |
| 提高指令上限到 2 亿 | **不是死循环** —— 跑满 1.5 亿条后崩 |

## 关键分界（这是最有用的线索）

| 命令 | 结果 |
| --- | --- |
| `python3.12 --version` | **正常退出**，590 万条指令 |
| `python3.12 -h` | **正常退出**，591 万条指令 |
| `python3.12 -c pass` | 1.5 亿条后崩 |
| `python3.12 -c 0` | 1.5 亿条后崩 |
| `python3.12 -X importtime -c pass` | 1.5 亿条后崩 |

**`--version`/`-h` 解析完参数就退出，不进解释器主循环；任何需要执行 Python 代码的入口都在同一处失败。**

## 一个可能相关的异常

**CPython 启动通常是五千万条指令量级，这里要 1.5 亿——大约三倍。**

而三个不同命令的指令数几乎相同（150.64M / 150.65M / 150.68M），
说明是**固定的初始化路径**，且这段路径上**有循环跑的次数偏多**。

**猜测：某个长度或偏移字段读错了，导致循环多转了几圈 —— 而这和最终那个坏指针，很可能是同一个根因。**

## 怎么复现

```bash
cd e2
make                      # 构建 tools/rune（解释器）
E2_CAP=200000000 ./tools/rune \
  /tmp/alpine-root/usr/bin/python3.12 -c "print(1)"
```

`/tmp/alpine-root` 是一个 Alpine aarch64 根文件系统（含 python3.12 + musl）。
`E2_CAP` 是指令上限，默认 6000 万（会在崩溃前就截断，看起来像死循环）。

## 现有的调试工具（都在 `e2/tools/`）

| 工具 | 用途 |
| --- | --- |
| `E2_WATCH=0x<addr>` | 监视一个 8 字节字，报告第一次变化时的 pc 和指令 |
| `E2_TRACE_CALLS=1` | 打印调用链（`[call] 0x调用方 -> 0x被调方`） |
| `E2_DUMP_REGIONS=1` | 崩溃时打印全部内存区域与权限 |
| `E2_TRACE_PCS=0x<addr>` | 从指定地址开始记录指令轨迹 |
| `tools/qemu-exectrace.py` | 把 qemu 的 `-d exec` 日志转成 guest PC 序列 |
| `tools/trace-align.py` | 两份轨迹做 LCS 对齐，报告第一个分歧点 |

## 已经做过的对齐工作（重要）

**两个实现的地址空间原本不同，现在已对齐到 qemu 的布局**：

- 程序加载在 `0x400000`
- 动态加载器 `ld-musl-aarch64.so.1` 加载在 `0x400000810000`
- 加载器入口 `0x4000008796bc` —— **和 qemu 的第一条 exec 轨迹地址完全一致**

**`tools/trace-align.py` 在前 2 万条指令上报告两边一致**（qemu 记基本块，本实现记每条指令，所以是子序列关系）。

**所以轨迹对比是可用的** —— 只要把范围推到 1.5 亿条附近。
但 `E2_TRACE_PCS` 的缓冲区只有 65536 条，需要先改大才能覆盖到崩溃点。

## 建议的下一步

1. **让 `E2_TRACE_PCS` 支持更大的缓冲区**（现在 `g_trace_buf[65536]`），
   或者支持「从地址 A 开始、记录 N 条」时用文件而不是内存缓冲
2. **在崩溃前的某个点抓两边状态对比** —— 特别是 `PyCode_Addr2Line` 的 `x0`（code object）
3. **查 `co_linetable` 是从哪里写进 code object 的**：
   结构体构造在 `0x28011c` 之前，追踪 `[code+0x88]` 的写入者
4. **顺带解释那 3 倍的指令数** —— 它可能指向同一个根因

## 相关文件

- `e2/src/exec.c` — 解释器（取指、解码 63 条 if 链、执行）
- `e2/src/syscall.c` — 系统调用层
- `e2/tools/rune.c` — harness（进程表、管道、信号、调试开关）
- `docs/E2-BACKEND.md` — 完整记录，最后一节是「五种反复出现的错误」

---

# 补充：对三条建议的核查（2026-11-21）

## ① 环形缓冲 / 不要按 1.5 亿窗口追踪 —— 同意

`E2_TRACE_PCS` 现在是 `static uint64_t g_trace_buf[65536]`，满了就 flush 并停止。
**改成故障时导出最后 N 条，是正确做法**，也避免 1.2 GB 的分配。

## ② `LDRSB` 的符号扩展 —— 已实测，实现是对的

**用独立小程序复现 `apply_load_opc(opc=3, bytes=1)` 的路径：**

```
读到: 0x80
w4   : 0xffffff80   bit31=1
结论 : TBZ 不跳转，循环退出 ✓
```

**`src/exec.c:98-105` 对 opc=3 的处理：**

```c
const unsigned bits = bytes * 8;          // 8
uint64_t extended = value;                 // 0x80
if (bits < 32) {
    const uint64_t sign = 1ULL << (bits - 1);   // 0x80
    extended = (value ^ sign) - sign;           // 0xFFFFFFFFFFFFFF80
}
value = (uint64_t)(int64_t)(int32_t)extended & 0xFFFFFFFFu;  // 0xFFFFFF80
```

**所以 `LDRSB` 不是原因。循环继续前进，是因为那段内存里每个字节的 bit31 确实都是 0
（ELF 头的零字节），不是因为它读错了符号。**

## ③ 「libpython 映射内」不能证明指针错 —— 同意，但这里不在映射内

**这条提醒是对的：3.11+ 的 deepfreeze 把代码对象放进静态数据，`co_linetable` 会指向
libpython 的 `.data`/`.rodata`，那是合法的。**

**但这次算下来，`co_linetable` 在基址**下方**：**

```
x6（数据终点）        = 0x10000013
x4 = co_linetable + 0x20
反汇编 280130: ldr x3, [x3, #0x10]   ; 长度
反汇编 28013c: add x3, x4, x3        ; 终点

若长度 = 0x00 → co_linetable = 0x0FFFFFF3   （基址 -0x0d）
若长度 = 0x13 → co_linetable = 0x0FFFFFE0   （基址 -0x20）
若长度 = 0x20 → co_linetable = 0x0FFFFFD3   （基址 -0x2d）
```

**三种可能都落在 `0x10000000` 之下 —— 那是未映射区，不是 `.data` 也不是 `.rodata`。**

**而 `0x0FFFFFE0` 恰好是「基址 - 0x20」= 「bytes 数据指针 - 头部大小」的形状，
很像某处把 `data_ptr` 当成了对象指针。**

**所以：坏指针的结论不变，但理由要改成「在映射之外」，而不是「在 libpython 里」。**

### 需要确认的一步

**上面三种可能取决于 `ob_size` 的实际值。因为 `[co_linetable + 0x10]` 读的是未映射地址，
本实现返回了什么需要查。若它返回 0（而不是 fault），长度就是 0。**

**这正是要查的点：`e2_memory_read` 对未映射地址应当返回非 0，`exec.c` 应当据此 fault。
但这次它没有在 `280130` 处 fault，而是在 `2fbbf4` 才 fault —— 说明那次读被当成了成功。**

**这可能是第二个缺陷：某条加载路径没有检查 `e2_memory_read` 的返回值。**

## 关于打包

已生成 `e2-for-codex.zip`（305 KB，51 个文件），包含：

```
e2/src/*.c  e2/include/e2/*.h  e2/tools/*.c *.py *.sh  e2/tests/  e2/Makefile
docs/M4-CPYTHON-BUG.md  docs/E2-BACKEND.md  docs/HANDOFF.md
repro.sh
```

**不含**：`/tmp/alpine-root`（49 MB 的 Alpine 根文件系统，需自行准备）。

### 补测结果（同一个调用内）

`PyCode_Addr2Line` 在整个崩溃运行中**只被调用一次**。在 `0x1028012c`（`ldr x3,[x3,#0x88]`
之后）转储：

```
x3  = 0x1081d150      ← co_linetable，看起来像指针
x1  = 0x7fffe970      ← 传给 _PyCode_CheckLineNumber 的局部结构
x19 = 0x107ecaa0
x6  = 0x10791970
sp  = 0x7fffe970
```

**而崩溃时的转储里 `x6 = 0x10000013`。**

**同一个寄存器，调用前是 `0x10791970`，崩溃时变成 `0x10000013` —— 说明中间被改写，
或者我抓到的是不同次调用（但计数器显示只有一次）。**

**这一点还没有解释清楚，是下一步最该查的地方：`x6` 在 `0x1028012c` 到 `0x102fbbf4`
之间如何从 `0x10791970` 变成 `0x10000013`。**

**注意 `0x1081d150` 和 `0x10791970` 都超出了区域表里 libpython 的范围
（`0x10000000 + 0x62e000 = 0x1062e000`）** —— **所以可能还有我没列出的映射，
或者这两个地址本身就是错的。`E2_DUMP_REGIONS` 只在崩溃时转储，需要改成同一时刻。**

## 新增工具：`E2_AT=0x<pc>`

**在指定 PC 处转储寄存器（最多 400 次）。比 `E2_WATCH` 更适合寄存器里的值——
watch 只能看内存地址，而这值一直在寄存器里。**

```
E2_AT=0x1028012c E2_CAP=200000000 ./tools/rune \
  /tmp/alpine-root/usr/bin/python3.12 -c pass
```

---

# 怎么拿到代码

## 首选：从 GitHub 直接 clone

**仓库是公开的，可以匿名克隆：**

```bash
git clone https://github.com/zqlnys/PocketDSH.git
cd PocketDSH/e2
```

**最新提交 `dcb3743`。这个仓库不需要 SSH，走 HTTPS。**

## 备选：离线 ZIP

**`e2-for-codex.zip`（313 KB，58 个文件）包含：**

```
e2/src/*.c             解释器、syscall、内存、加载器
e2/include/e2/*.h
e2/tools/*             rune.c 之外的调试工具 + trace-align.py + qemu-exectrace.py
e2/tests/main.c        单指令测试（全部）
e2/Makefile
docs/M4-CPYTHON-BUG.md 问题报告
docs/E2-BACKEND.md     完整记录
docs/HANDOFF.md
docs/SSH-ACCESS.md
repro.sh
```

## 能跑什么、不能跑什么（重要）

**`make test` 不需要 Alpine 根文件系统** —— 这些测试用的 guest ELF 是现场用
`clang` + `ld.lld` 交叉编译出来的。

```bash
cd e2
make test        # → 全部通过（无失败）
```

**只依赖 `clang` 和 `ld.lld` 两个工具。**

**不能直接复现 CPython 崩溃** —— 那需要 `/tmp/alpine-root`（49 MB 的 Alpine aarch64
根文件系统，含 python3.12 和 musl）。**这个没有打包。**

**所以：改解释器 → `make test` 验证 → 我在这边用真实的 Alpine 根跑 CPython 确认。**

## 建议的改动顺序

1. **`E2_TRACE_PCS` 缓冲区改成环形**（`g_trace_buf[65536]` → 故障时导出最后 N 条）。
   **这条不依赖 Alpine，改完 `make test` 就能验证。**
2. **修 `x6` 从 `0x10791970` 变成 `0x10000013` 的问题**——需要真实运行才能观察，
   但代码改动可以用 `make test` 兜底。
3. 环回后我这边跑 CPython，把新轨迹给你。

## 当前最具体的问题（复述）

**`PyCode_Addr2Line` 只被调用一次。在 `0x1028012c`（刚 load 完 `co_linetable`）处：**

```
x3 = 0x1081d150     x6 = 0x10791970
```

**崩溃时（`0x102fbbf4`）：**

```
x6 = 0x10000013
```

**同一个调用、同一个寄存器，值变了。中间只有 `0x1028012c → 0x10280148 → 被调函数`
这一段。**

**已经排除**：`LDRSB` 符号扩展是对的（实测 `0x80 → 0xffffff80`，bit31 = 1）；
`co_linetable` 指向 libpython 这件事本身不能证明有错（冻结对象合法）。

**所以要么 `x6` 在这段被改写，要么我抓到的转储不属于同一次调用（但计数器显示只有一次）。**

---

# 2026-11-21 更新：`co_linetable` 是好的，扫描起点是错的

## 实测到的内存布局

**在 `0x102fbbf0`（第二个反向扫描的入口）dump 出：**

```
0x1081d160:  58 0d 00 00 00 00 00 00    ← ob_size = 0x0d58 = 3416
0x1081d168:  ff ff ff ff ff ff ff ff    ← 对象头字段
0x1081d170:  f0 03 01 01 01 f0 5c 05    ← 行表数据起点
```

**结构体的两个字段（实测）：**

```
[x19+0x18] = 0x1081d170    ← start（每次调用递减）
[x19+0x20] = 0x1081dec8    ← end（不变）
```

**`0x1081dec8 - 0x1081d170 = 0x0d58 = 3416` —— 和 `ob_size` 完全吻合。**

**所以 `co_linetable = 0x1081d150` 是合法的 bytes 对象，start/end 也都正确。
之前「坏指针」的结论作废。**

## 真正的问题：扫描从 start 开始，而且只会递减

**循环（`_PyCode_CheckLineNumber`）：**

```asm
1e7a78: ldp  x2, x1, [x19, #0x18]   ; x2 = start, x1 = end
1e7a7c: cmp  x2, x1
1e7a80: b.hs 0x1e7abc               ; start >= end 才退出
1e7a84: bl   0x1e7ad0               ; 解码一条
```

**而解码过程中调用的扫描函数把 `[x19+0x18]`（即 x2）**减小**：**

```asm
2fbbc4: sub  x0, x0, #1
2fbbc8: str  x0, [x5, #0x18]        ; start -= 1
```

**start 只会变小，`start >= end` 永远不成立 → 无限循环。**

**而且起点是 `0x1081d170`（数据**开头**），往前退会走进对象头的 `ff` 字节，
再往下就越过整个映射。**

**`0x1081d170 → 0x10000000` 是 850 万字节，每字节约 18 条指令 ≈ 1.5 亿条 —— 
和实测的 `instructions=150636853` 完全对上。**

## 待查的方向

**两个可能，需要 Codex 判断哪个：**

1. **`stp x4, x3, [sp, #0x18]` 的顺序**：若 CPython 期望 `{end, start}` 而我读到的是
   `{start, end}`，那扫描就该从 end 开始。但这是 libpython 自己的机器码，
   qemu 下同一份二进制是对的，所以更可能是**寄存器值**在到达 `0x280144` 前就不对。

2. **`x4` / `x3` 的算法**：
   ```asm
   28012c: add x4, x3, #0x20    ; x4 = co_linetable + 0x20
   280130: ldr x3, [x3, #0x10]  ; x3 = ob_size
   28013c: add x3, x4, x3       ; x3 = start + size
   ```
   **实测 `x4 = 0x1081d170`、`x3 = 0x1081dec8`，两个都对。**
   **所以这一步没问题 —— 问题在被调函数如何使用它们。**

3. **`ldursb w1, [x6, #-1]` 与 `tbz w1, #31`**：
   **在 `0x1081d170` 处往前读，字节是 `0xff`（bit31 = 1），应当继续；
   再往前到 `0x1081d167` 是 `0x00`（bit31 = 0），应当停止。**
   **但从调用次数看，每次调用只让游标减 1 —— 说明内层循环每次都立即退出了。**
   **这可能是 `LDURSB` 或 `TBZ` 的模拟问题，值得单独验。**

## 相关工具

**`E2_AT=0x<pc>` 现在会转储：**
- 通用寄存器
- `[x19+0x18]` 和 `[x19+0x20]`（结构体的两个指针）
- `x6-16` 起 24 个字节的原始内容

```
E2_AT=0x102fbbf0 E2_CAP=200000000 ./tools/rune \
  /tmp/alpine-root/usr/bin/python3.12 -c pass
```

## 排除项：`LDRSB` + `TBZ` 组合是对的

**把 CPython 那个循环的两条核心指令单独跑了一遍：**

```asm
ldrsb w4, [x1]
tbz   w4, #31, skip
```

**实测：**

```
输入 0x7f → w0 = 2（bit31 清零，TBZ 跳转）   ✓
输入 0xff → w0 = 1（bit31 置位，TBZ 不跳）   ✓
```

**已固化为单测 `test_ldrsb_and_tbz_together`，并验证判别力：**
- 变异「TBZ 跳转条件反了」→ **CAUGHT**
- 变异「LDRSB 不符号扩展」→ **CAUGHT**

**所以指令模拟不是原因。** 问题在别处：

**「扫描只减不增，而循环条件要求它增大才能退出」——
这个矛盾只能靠 CPython 源码里 `advance()` 的真实语义来解释，
而这需要读 3.12 的 `Objects/codeobject.c`，我这边没有源码。**

**需要的输入**：`_PyCode_CheckLineNumber` 和 `advance()` 在 3.12.13 的源码。

---

# 2026-11-21 晚：根因确定 —— `MOVI` 的立即数展开错误

## 一句话

**`movi d0, #0x000000ffffffff`（编码 `0x2F00E5E0`）在本实现里得到
`0x0f0f0f0f0f0f0f0f`，正确值是 `0x00000000ffffffff`。**

## 这如何导致 CPython 崩溃

**`PyCode_Addr2Line` 用这条指令初始化地址范围结构体：**

```c
_PyLineTable_InitAddressRange(...):
    range->ar_start = -1;      // 和 ar_end 一起由 movi d0, #0x000000ffffffff 写入
    range->ar_end = 0;
```

**`movi d0` 写入 8 字节：`ar_start`（+0）= 低 32 位，`ar_end`（+4）= 高 32 位。**

**本实现给出 `0x0f0f0f0f0f0f0f0f`，于是：**

```
ar_start = 0x0F0F0F0F = 252645135    （应为 -1）
ar_end   = 0x0F0F0F0F = 252645135    （应为 0）
```

**实测确认（`E2_AT=0x102fbbf0`）：**

```
[at] ar_start=252645135 ar_end=252645135 ar_line=252645135 lasti=2078
[at] ar_start=252645119 ar_end=252645119 ar_line=-1          lasti=2078
```

## 为什么跑 1.5 亿条指令

**`_PyCode_CheckLineNumber` 的循环（CPython 3.12 源码）：**

```c
while (bounds->ar_start > lasti) {
    if (!_PyLineTable_PreviousAddressRange(bounds)) return -1;
}
```

**`ar_start = 252645135 > 2078` 恒成立，每次减 16 或 1，
要走约 1580 万次才降到 2078 —— 与实测的 1.5 亿条指令吻合。**

## 现有实现的错误

**`src/exec.c` 里 cmode 的注释表写的是：**

```
*  0xE    0   8        -
*  0xE    1   64       -
```

**两行都不对。** 架构定义里 `cmode<3:1> = 111` 是「8 位，MSL #8」，
`cmode<0>` 决定是否移位；64 位形式还取决于 `Q` 和 `op`。

## 修复方向

**按 ARM `AdvSIMDExpandImm` 的伪代码重写，用 llvm 生成对照表验证。**

**已知的四条地面真值（llvm 汇编得到）：**

| 编码 | 汇编 | 正确值 |
| --- | --- | --- |
| `0x2F00E5E0` | `movi d0, #0x000000ffffffff` | `0x00000000ffffffff` |
| `0x2F00E420` | `movi d0, #0xff` | `0xffffffffffffffff` |
| `0x2F02E6A0` | `movi d0, #0x00ff00ff00ff00ff` | `0x00ff00ff00ff00ff` |
| `0x2F00E460` | `movi d0, #0x000000000000ffff` | `0x000000000000ffff` |

## 为什么这条缺陷一直没被抓到

**248 项单测里没有一条覆盖 SIMD 立即数展开**，而 374 条差分用例跑的 busybox
也没用到 `movi` 的这些形式 —— 它用的是 16 位或 32 位立即数，恰好落在实现正确的分支上。

**`movi d0, #0x000000ffffffff` 这种「32 位全 F 复制两次」的形式是 CPython 特有的用法。**

---

# 2026-11-21：修好了，CPython 从崩溃变成正常报错

## 修复

```c
} else if (cmode == 0xE && op) {
    // 每个 imm8 的置位对应一个 0xFF 字节
    uint64_t pattern = 0;
    for (unsigned i = 0; i < 8; i++) {
        if ((imm8 >> i) & 1u) pattern |= 0xFFULL << (8 * i);
    }
```

**验证方式**：用 llvm 生成 255 条 d-form 编码的真值，全部吻合。

**并固化为单测 `test_movi_64bit_immediate`（4 条用例），变异验证判力：**
把展开改回「复制整个字节」→ **CAUGHT**。

## 效果

| 阶段 | 指令数 | 结果 |
| --- | --- | --- |
| 修复前 | 150,636,853 | FAULT：走到映射外 |
| **修复后** | **15,168,740** | **正常退出，Python 层报错** |

**指令数降到十分之一，而且不再是解释器崩溃。**

## 现在的报错（下一个问题）

```
Exception ignored error evaluating path:
Traceback (most recent call last):
  File "<frozen getpath>", line 490, in <module>
ValueError: embedded null character
Fatal Python error: error evaluating path
Python runtime state: core initialized
```

**`<frozen getpath>` 是 CPython 用冻结的 Python 代码算 sys.path——
它能跑到这里，说明解释器、`getpath` 的 Python 部分、以及大部分 C 运行时都已经正确。**

## 顺带修的第二个缺陷

**`readlink /proc/self/exe` 之前返回空**（qemu 返回 `/bin/busybox`）。
**现已合成：返回当前程序的 guest 路径。**

**但 trace 显示 CPython 只调用了一次 readlink，且路径是空的——**
**说明 NUL 来自别处，还没定位。**

## 已确认正确的地方

- `movi d0` 的各种形式（255 条）
- `readlink /proc/self/exe` 返回 `/usr/bin/python3.12`
- 不存在的文件 `open` 正确返回 ENOENT
- `getcwd`、`pwd` 正确

## 下一步

**`real_executable_dir` 里带 NUL，导致 `joinpath` 产生带 NUL 的路径，
`open` 在发出系统调用之前就抛 `ValueError`——所以它不被 `except FileNotFoundError` 捕获。**

**要查 `real_executable` 是怎么算出来的：它在 `getpath` 的 C 部分（`Modules/getpath.c`），
依赖 `readlink("/proc/self/exe")` 或 `AT_EXECFN`。**

## 下一层：argv[0] 与宽字符

### 发现 1：`ValueError: embedded null character` 的成因

**`readlines` 在 C 侧是 `getpath_readlines`，它调用 `PyUnicode_AsWideCharString(pathobj)`——
对含内嵌 NUL 的字符串会抛 `ValueError: embedded null character`。**

**那条路径来自 `joinpath(real_executable_dir, 'pybuilddir.txt')`，
`real_executable_dir` 之所以是空的（于是 `abspath('.')` = `/`），
是因为 CPython 判定 `executable` 为空。**

**根因：`argv[0]` 传的是 `"python3.12"`（basename），里面没有 `/`。**

**CPython 的 getpath.py：**

```python
if not executable and SEP in program_name:
    executable = abspath(program_name)
...
if not executable:
    executable = ''
    executable_dir = abspath('.')     # ← 落到这里
```

**用 `E2_ARGV0=/usr/bin/python3.12` 传完整路径后，NUL 错误消失。**

### 发现 2：新错误 —— 宽字符解码

```
ValueError: character U+7275732f is not in range [U+0000; U+10ffff]
```

**`0x7275732f` 按小端拆开是字节 `2f 73 75 72` = `"/sur"`。**

**四个 ASCII 字节被当成一个 `wchar_t` 读了出来。**

**aarch64 Linux 上 `wchar_t` 是 4 字节 UTF-32。所以某处把字节写进了宽字符缓冲区。**

**下一步查 `Py_DecodeLocale` / `PyUnicode_AsWideCharString` 用到的
`mbrtowc` / `mbstate_t` 语义是否与 musl 一致。**

### 当前调用序列

```
[open] "/etc/ld-musl-aarch64.path"
[open] "/lib/libpython3.12.so.1.0"
[open] "/usr/local/lib/libpython3.12.so.1.0"
[open] "/usr/lib/libpython3.12.so.1.0"
→ ValueError: character U+7275732f ...
```

**`open` 的路径现在全是绝对路径——说明 `executable_dir` 已正确推导。**

---

# 2026-11-21：`USHLL2` 的目标索引错误 —— 宽字符问题的根因

## 缺陷

```c
// 错：目标索引用了源 lane
const int half = (src_idx * dsize) / 64;

// 对：用目标 lane
const int half = (i * dsize) / 64;
```

**`USHLL2` 带 Q 时从源的**上半**取元素，但结果始终从 lane 0 开始写。
用 `src_idx` 当目标索引，`i=0` 时 `half = 2` —— **越过 `cpu->v[rd][0..1]` 的边界，
什么也没写，调用方读到的是那里原有的数据。**

## 这如何毁掉 CPython

**CPython 的 `PyUnicode_AsWideCharString` 用向量化循环把 ASCII 串拓宽成 `wchar_t`：**

```asm
233cd8: ushll2 v1.4s, v1.8h, #0x0
233cdc: ushll2 v0.4s, v0.8h, #0x0
233ce0: stp q3, q1, [x0]
```

**拓宽失效后，目标缓冲区里留着调用前的内容——而那是源字符串本身按字节排布的结果，
于是 `"/sur"` 四个字节被当成一个 `wchar_t` 读出来：**

```
ValueError: character U+7275732f is not in range [U+0000; U+10ffff]
```

## 效果

| 阶段 | 结果 |
| --- | --- |
| 修 `movi` 之前 | 1.5 亿条指令后 FAULT |
| 修 `movi` 之后 | 1.5 千万条后 `ValueError: embedded null character` |
| 修 `argv[0]` 之后 | 同上错误消失，变成宽字符错误 |
| **修 `ushll2` 之后** | **宽字符错误消失，新的 FAULT 在 `0x1017a884`** |

**每一层都更靠后、更接近 Python 真正跑起来。**

## 为什么之前没抓到

**`USHLL`/`SSHLL` 的注释说「先读整个源再写，否则会破坏源」——
这个考虑是对的，但目标索引那一步错了，而错的后果是「静默不写」，
差分用例和单测都没有覆盖带 Q 的形式。**

**已加单测 `test_ushll2_widens_from_the_upper_half`，变异改回 `src_idx` → CAUGHT。**

## 位域指令是对的 —— 用真实执行验证过

上一轮我重写了 `UBFM`/`SBFM`/`BFM`，用自写的 Python 参考模型生成 233 条对照，
得到「0/233 不符」。**那个绿色是假的**：模型和实现都出自我一个人，
两边一起错的时候它们会互相印证。

**回退之后，改用真实执行做对照：**

**写一个 guest ELF（`tests/guest/bitfield.s`），里面跑 50 条位域指令并打印结果，
在 qemu 和本实现下各跑一次。**

```
$ chroot ... /tmp/qemu bf.elf   > q.txt
$ ./tools/rune bf.elf           > m.txt
$ diff q.txt m.txt
50 条完全一致
```

**所以原来的位域实现是正确的，我的「重写」才是错的。**

### 这个方法才是可用的

**已固化为 `tests/guest/bitfield.elf` + `test_bitfield_matches_a_real_run`，
期望值直接来自 qemu 的输出，而不是我对架构规范的理解。**

**同一条路已经验证过 `shll.elf`：`1,-1|16,-16|1,4294967295` 两边一致。**

### 一条方法上的教训

| 对照物 | 可信度 |
| --- | --- |
| 自写的参考模型 | **不可信**——同源错误会互相印证 |
| llvm 的汇编/反汇编 | 可信（用于 `movi`、`ushll2` 的定位）|
| **qemu 实跑同一 ELF** | **最可信**——就是真机行为 |

**以后验证指令语义，一律走第三条。**

---

# 2026-11-21 深夜：帧指针差了整整一个字节

## 故障

```
FAULT addr=0x105bf5b010 pc=0x1017a884 insn=0xf9400823 (ldr x3,[x1,#0x10])
x1=0x105bf5b000  x5=0x106b438f  x24=0x10806068
函数：_PyEval_EvalFrameDefault
```

**`x5` 是 CPython 求值循环的帧指针，它指向 `0x106b438f`——一个**奇数**地址。**

## 决定性证据

**在同一个地址 `0x106b438f` 和它附近 8 字节对齐的基址上，分别读 `+0x28` 字段：**

```
[0x106b4380 + 0x28] = 0x106ebf00   合法（指向 libpython）
[0x106b4388 + 0x28] = 0x106ebf40   合法
[0x106b4390 + 0x28] = 0x105bf5b0   合法  ← 正确的帧
[0x106b4398 + 0x28] = 0x100000007
```

**而实际用的 `0x106b438f` 读出 `0x105bf5b000` —— 一个未映射地址。**

**再看原始字节（`0x106b438f` 起）：**

```
+0x28: 00 b0 f5 5b 10 00 00 00
```

**同一条字节串从后一个字节读：`b0 f5 5b 10 00 00 00 ??` = `0x105bf5b0`。**

**所以：结构体本身没问题，是帧指针 `x5` 比正确值小了 1。**

**`0x106b4390 - 0x106b438f = 1`。**

## 这意味着什么

**整条求值链路都在读一块「提前一个字节」的结构体，于是每个字段都是错的——
`[x5+0x28]` 拿到的不是 `f_code` 之类，而是相邻字段错位后的字节。**

**这个 -1 的来源还没找到：**
- `E2_WATCH=0x10806068`（存 x5 的那个字段）没有捕获到写入，
  说明该值在监视开始前就已存在，或者写入路径绕过了监视
- 指针运算出现 ±1 不是 AArch64 栈操作的常见形态（栈是 8/16 字节粒度）

**下一步：查 `E2_WATCH` 是否真的覆盖所有写路径，然后重放这段写入。**

## 新增工具

**`E2_DUMP_AT_FAULT=1` 在崩溃时转储：**
- `x5` / `x24` / `x19` / `x1` 指向的内容
- `x5` 附近四个 8 字节对齐基址上的 `+0x28` 字段（用来判断是「指针错」还是「内容错」）
- `x24` 前后 40 字节的原始内容

**这三组信息正是本轮定位这个 -1 的依据。**

## 那个 -1 是「对象地址少了 1」，用 `ob_type` 可以确证

**帧里存的不是帧指针，而是 `f_func`（一个 `PyFunctionObject*`）。**

**把同一块内存按两个基址解读，看 `ob_type` 字段（对象头 +0x08）：**

```
按 0x106b438f 解读:  [+0x08] = 0x1059c63000   ← 无效
按 0x106b4390 解读:  [+0x08] = 0x1059c630     ← 合法的 PyTypeObject 地址
```

**`0x1059c630` 在 libpython 内，是个合理的类型对象；`0x1059c63000` 不是。**

**所以对象真实地址是 `0x106b4390`，而帧里写的是 `0x106b438f` —— 少了 1。**

**同时 `x5` 的四个对齐候选里，只有 `0x106b4390+0x28` 给出合法指针 `0x105bf5b0`，
与这一结论一致。**

## 已验证的事实

**`0x10806068`（帧里存 `f_func` 的位置）确实在映射区内（region 60: `0x10806000 + 0x4000`），
所以不是「越界读到垃圾」。**

**`E2_WATCH=0x10806068` 在整次运行里没有捕获到写入 —— 这一点目前无法解释：
要么该写入发生在监视开始之前，要么它绕过了逐条指令的检查。
`E2_WATCH` 的实现是每条指令后读一次比较，因此第二种可能意味着写入路径不经过
`e2_memory_write`。**

**下一步应该是：先确认 `E2_WATCH` 覆盖所有写路径，再重放这段写入，看是哪条指令把
`0x106b4390` 变成了 `0x106b438f`。**

---

# 2026-11-21：抓到了——一次 `Py_DECREF` 减在指针上

## 上一轮为什么「没看到写入」

**`E2_WATCH` 有个 8 次上限，用完就自动关闭。那个帧字段在启动阶段就变了 8 次，
监视早早失效，而真正的破坏发生在很久之后。**

**我当时的 `grep -E "WATCH|FAULT"` 还是大写，而输出是小写 `[watch]`——
两个错误叠在一起，让我以为「从未写入」。**

**现在 `E2_WATCH_MAX` 可配置，默认仍是 8。**

## 抓到的那两次写入

```
0x0        -> 0x106b4390   pc=0x10176ae8  str x21, [x19]   ← 正确值
0x106b4390 -> 0x106b438f   pc=0x1017b018  str x1,  [x0]    ← 被减 1
```

## 那条指令是 `Py_DECREF`

```asm
17b004: ldr  x0, [x27]
17b008: ldr  x1, [x0]        ; x1 = 引用计数
17b00c: tbnz w1, #0x1f, ...  ; 不死对象则跳过
17b010: sub  x1, x1, #1      ; 计数减一
17b014: str  x1, [x0]        ; 写回
17b018: cbz  x1, ...         ; 归零则释放
```

**寄存器实测：**

```
正常: [at] pc=0x1017b018  x0=0x106ef780   x1=0x0          ← 真对象，计数归零
异常: [at] pc=0x1017b018  x0=0x10806068   x1=0x106b438f   ← 把指针当计数
```

**`x0 = 0x10806068`，而 `[x0] = 0x106b4390` 是 `f_func` 指针（`PyFunctionObject*`），
不是引用计数。`sub #1` 把它变成 `0x106b438f`，函数对象地址就此损坏。**

**之后求值循环用这个坏地址当 `f_func`，`[x5+0x28]` 读到未映射的 `0x105bf5b000`，崩溃。**

## 剩下要查的

**`x0` 本应是对象指针，实际是「帧里 `f_func` 字段的地址」——
多了一层间接。也就是某处把 `&slot` 当成了 `slot`。**

**需要看 `x27`：`ldr x0, [x27]`，`[x27] = 0x10806068`。
要弄清是什么把这个地址写进了 `[x27]` 指向的槽位。**

**`E2_AT` 目前不转储 `x27`，下一步先把它加上。**

## 再往下：写入 `0x10806068` 的那条指令

**监视 `0x108060d0`（`x27` 指向的槽位）得到：**

```
0x106ec400 -> 0x10806068  pc=0x1017b2d8  insn=0xf9000713  (str x19, [x24, #0x8])
```

**所以是 `str x19, [x24, #0x8]` 把帧指针 `0x10806068` 写进了那个槽位。**

**往上看这段：**

```asm
17b2b8: add  x0, x19, #0x48
17b2c4: str  x20, [x19, #0x38]
17b2c8: strh wzr, [x19, #0x44]
17b2d0: str  w0,  [x19, #0x40]
17b2d4: str  x19, [x24, #0x8]     ← 帧指针存进 [x24+8]
```

**这是在初始化一个帧对象，然后把帧指针存到 `[x24+8]`——`x24` 在那一刻是线程状态。**

**紧接着 `0x17b004` 的 `Py_DECREF` 从同一个槽位把它读出来，当作对象去减引用计数。**

## 目前的自洽性缺口

**`x24` 在不同时刻取值不同：**

```
E2_AT 在 0x1017b018 处: x24 = 0x108060e0
写入点 0x1017b2d8 处:   x24 = 0x108060c8（因为 [x24+8] = 0x108060d0）
```

**同一个寄存器既是帧指针又是线程状态，说明我还读错了某处——
要么 `x27` 在 0x17b004 和 0x17b018 之间被改过，要么这条 DECREF 的调用约定与我的理解不同。**

**在把这个缺口填上之前，不应再基于「DECREF 减错对象」下结论。**

## 已确定无疑的事实

- **`0x106b4390` 是合法的 `PyFunctionObject`（`ob_type = 0x1059c630`）**
- **它被 `sub #1` 变成 `0x106b438f`，此后再被当作 `f_func` 使用**
- **这一步发生在 `0x17b010`/`0x17b014`，指令是 `sub x1,x1,#1; str x1,[x0]`**
- **`x0` 来自 `ldr x0,[x27]`，而 `x27` 指向的槽位当时存的是帧指针**

**下一步：先填 `x24` 的矛盾，再判断是 CPython 的哪段逻辑走到了这里。**

## `x24` 的矛盾解释了：那是 `CALL` 之后的参数清理

**在 `0x17b004` 处观测到的完整上下文：**

```asm
17afd4: bl   PyObject_Vectorcall
17afd8: mov  x23, x0
17afe8: ldr  x0, [x26]        ; DECREF 可调用对象
17aff0: sub  x0, x0, #1
17aff4: str  x0, [x26]
17b004: ldr  x0, [x27]        ; DECREF 参数
17b008: ldr  x1, [x0]         ; 引用计数
17b010: sub  x1, x1, #1
17b014: str  x1, [x0]
```

**所以 `x27` 是参数向量的指针，`[x27]` 是第一个参数。**

**而那个位置存的是 `0x10806068` —— 帧地址，不是对象。**

**`x24` 的「矛盾」也就说得通了：`x24` 是帧指针，`x27 = x24 - 0x10`，
两者相差 0x10 是参数区与帧头的固定偏移。之前把两次不同迭代的数据并排放，才以为矛盾。**

## 已验证：`LDP`/`STP` 不是原因

**`CALL` 路径靠 `LDP`/`STP` 搬运参数，所以先排除了它。**

**用 qemu 与本实现跑同一个 guest ELF（`tests/guest/ldp.s`），
覆盖 `stp/ldp` 的无偏移、前变址、后变址三种形式：**

```
0000000000000011 0000000220254 0000000220204 0000000000000033 ...
两边完全一致
```

**已固化为 `test_ldp_stp_match_a_real_run`。**

## 下一步

**参数槽 `[x27]` 里的 `0x10806068` 是帧地址，说明构造参数向量时写进了错误的指针。
需要查 `CALL` 之前把参数搬进 `localsplus`/`args` 的那段代码，
看它用的是哪条指令，再用 qemu 对照验证。**

---

# 2026-11-21：工具链升级 —— 能问到 qemu 的真实状态了

## 1. 地址布局终于和 qemu 一致

**qemu-user 把所有 guest 地址放在它保留的 `guest_base = 0x4000000000` 里：
loader 在 `0x400000810000`，libpython 在 `0x4000008e5000`。**

**本实现之前只有 loader 对上了，匿名映射却从 `0x10000000` 起：

```
改前: libpython 0x10000000     loader 0x400000810000    ← 两套地址空间
改后: libpython 0x4000000000   loader 0x400000810000    ← 统一
```

**`e2_memory_map_any` 的起始候选从 `0x10000000` 改成 `0x4000000000`。
265 项单测、374 条差分、M2/M3 全绿。**

**故障点相对位置不变（仍在 `0x17a884`），但地址可比了——
这是后面所有工具的前提。**

## 2. 自己写的 qemu gdb 客户端

**系统没有 gdb，但 qemu 支持 `-g <port>`。`tools/qprobe.py` 实现了最小 GDB RSP 客户端：**

- `qSupported` / `?` / `g` / `m` / `Z0` / `c`
- `--at` 断点、`--until reg=value` 条件、`--regs`、`--mem`

**两个坑：**

1. **qemu 不会主动发停止包**，必须先发 `?`
2. **寄存器包是 268 字节**（536 个十六进制字符）：`x0-x30` + `sp` + `pc` 各 8 字节，
   `cpsr` 只有 4 字节。按「34 个 8 字节」读会短 4 字节。

**用法：**

```
qemu -g 12399 ... &
tools/qprobe.py --port 12399 --at 0x400000a60004 --regs x0,x24,x27
```

## 3. 第一次拿到「同一 PC 上两边都正确」的对比

**在 `0x17b004`（那条 `Py_DECREF`）上：**

| | qemu | 本实现 |
| --- | --- | --- |
| `[x27]` | `0x400000f99120`（堆里的对象）| `0x10806068`（帧地址）|
| `[[x27]]` | **`0x1`（正常引用计数）** | `0x106b4390`（被当成计数减掉）|

**但两边停在不同的字节码上（`x19`/`x20` 不同），所以这不是同一次调用，
不能据此下结论。这一点很关键——上一轮我就差点又误判。**

## 4. 轨迹对齐：分歧在词法分析器

**新增 `tools/trace-diverge.py`，把两边轨迹减去各自的 libpython 基址后对齐。**

**qemu 侧用 `-d exec` 生成（约 1.8M 个基本块），本实现用 `E2_TRACE_PCS`。
轨迹缓冲从固定 64K 改成 `E2_TRACE_PCS_MAX` 可配，否则看不到 6 万条之后的分歧。**

**第一次对齐结果（从求值循环入口 `0x17a5f0` 起）：**

```
qemu entry 6291 / 本实现 entry 157736
  qemu 期望 0x18fcf8
  本实现有  0x18fd1c
```

**对应指令：**

```asm
18fd18: cmp x3, x4
18fd1c: b.lt 0x18fcf8     ← qemu 跳了，本实现没跳
18fd20: cmp x24, x23
```

**在 `_PyToken_OneChar`（词法分析器）里。**

**但采样到的 `x3` 恒为 `0x1ff`、`x4` 在 0x0f..0xf6 之间，
与「跳转需要 x3 < x4」矛盾——说明采样到的调用实例仍不是分歧那一次。
需要按「第 N 次命中」定位，这是下一步。**

## 本轮教训（又一次同一类）

**「在不同调用实例上比较寄存器」已经骗了我两次。
比较必须在同一次调用上，否则差异全是噪声。
`--until` 条件断点能做，但每次命中要两个往返包，太慢；
更快的做法是让两端各自记录「第 N 次命中」的寄存器。**

---

# 2026-11-21：逐指令对齐，把「不可比」变成了「可比」

## 方法

**qemu 的 `-d exec` 只记基本块入口，用它对齐会得到假分歧（上一轮就上当了）。
改用 `-singlestep -d exec` 得到**逐指令**轨迹（约 3900 万条），
再把两边各自减去自己的 libpython 基址后逐条比较。**

**两边的环境必须一致，否则第一批分歧来自环境变量个数——`_dlstart` 会扫 envp。**

```
env -i  chroot … qemu -singlestep -d exec …   → 39,618,891 条
E2_MIN_ENV=1 ./tools/rune … E2_TRACE_PCS=…    →  逐指令 PC
```

## 结果：对齐逐步推进

| 轮次 | 一致到第 N 条 | 分歧原因 |
| --- | --- | --- |
| 初始 | 255 | auxv 条目数不同（17 vs 19）|
| 补 `AT_PLATFORM`、`AT_MINSIGSTKSZ` | 1653 | `AT_PAGESZ` 的位置不同 |
| 按 qemu 顺序重排 auxv | **9565** | `AT_HWCAP` 的位置 |

## 这意味着什么

**前 9565 条指令里，本实现与 qemu 逐指令完全一致——包括 musl 的
`_dlstart`、重定位、以及 loader 的绝大部分。这是第一次把「loader 阶段是否正确」
从推断变成了实测。**

**剩下的分歧都还在 auxv 的**顺序**上，不在指令模拟上。**

## 仍不可靠的地方

**自己写的 auxv 探针（`tests/guest/auxv.s`）在 qemu 下输出有重复类型，
说明它的读数不对，不能作为 qemu auxv 顺序的依据。要匹配顺序，
得先把探针修对，或者用 RSP 读初始栈（读取偶发失败，需要重试）。**

## 工具

- **`E2_AT_NTH=<n>`**：只报告第 n 次命中。此前「比较不同调用实例的寄存器」
  已经骗了三轮，这是根治。
- **`E2_TRACE_PCS_MAX`**：轨迹缓冲不再固定 64K。
- **`tools/trace-diverge.py`**：带基址归一化的分歧定位。

---

# 2026-11-21：轨迹对齐走到尽头，以及一个精确的 DECREF 定位

## 逐指令对齐的极限

**用「从求值循环入口开始、逐条比对」的方法，把分歧定位到第 156,520 条指令：**

```
分歧: qemu 0x18fc74   mine 0x18fd54
```

**对应代码是哈希表探测：**

```asm
18fc60: ldr  x19, [x6, x4]
18fc64: add  x4, x20, #0x1
18fc68: cmp  x19, #0x0
18fc6c: ccmp x19, x21, #0x4, ne    ← 指针身份比较
18fc70: b.ne 0x18fd54
```

**固定 `PYTHONHASHSEED=0` 后分歧位置一模一样，说明不是哈希随机化。**

**真正的原因是 `ccmp x19, x21` 比较的是**对象指针身份**，
而两次运行的堆布局不同，指针本来就不会相等。**

**结论：逐指令轨迹对比在第一次「指针身份比较」处就到头了，
除非两边的堆分配完全一致。这条路到此为止——这是个有价值的负面结果，
省得以后再往里投时间。**

## 一个精确的 DECREF 定位

**新增 `E2_CHECK_DECREF=1`：在 `CALL` 后的参数清理处检查 `[x27]` 是否为对象
（首字应是引用计数），第一次出现「首字本身是 guest 指针」时报告。**

**第一次命中的完整现场：**

```
x27  = 0x40008060d0   = 帧 - 0x10
[x27]= 0x4000806068   = 帧 - 0x78      ← 参数槽里是「槽的地址」
[[x27]]= 0x40006b4390                  ← 函数对象被当作引用计数减掉
x24  = 0x40008060e0   （帧）
x20  = 0x2            （参数个数）
```

**监视两个地址得到的完整写入链：**

```
0x4000806068: 0x0 -> 0x40006b4390  pc=0x4000176ae8  str x21,[x19]   ← 正确写入函数对象
0x40008060d0: … -> 0x4000806068  pc=0x400017b2d8  str x19,[x24,#8] ← _PyCFrame.current_frame
```

**所以：函数对象在 `0x4000806068` 处是**对的**；
问题在于 `x27` 应该指向 `0x4000806068`（内容是对象），却指向了 `0x40008060d0`（内容是那个地址）——
**多了 0x68 的偏移。**

**下一步就是查 `x27` 在这个 CALL 路径上的计算。**

## 一个自己踩的坑

**新加的断言最初写成裸 `getenv` 放在热循环里，性能立刻从 99 ns 掉到 142 ns——
正是上一轮刚写进文档的那个错误。已改用 `env_once`，回到 86 ns。**

## 「按执行次数对比」也不成立

**失败的那次 DECREF 是第 107 次执行。于是去 qemu 里看第 107 次：**

| | qemu | 本实现 |
| --- | --- | --- |
| `x19` / `x20` | 3 / 3 | 1 / 2 |
| `x24` | `0x4000010eb090` | `0x40008060e0` |
| `x27` | `x24 - 0x18` | `x24 - 0x10` |

**`x19`/`x20` 不同说明停在不同字节码上——两边的执行在更早就分岔了，
就是那条指针身份比较（`ccmp x19,x21`）。**

**所以「按第 N 次执行配对」这条路也堵住了：只要程序里有任意一次指针身份比较，
两次运行的指令序列就无法长期对齐。**

## 那个「参数」到底是什么

**在失败点检查槽位的对象头：**

```
slot = 0x4000806068
[slot] = 0x40006b4390          → 是个 function 对象
[slot+0x20] = 0x40006e8cc0     → 是个 dict，不是 f_code
```

**所以 `0x4000806068` 不是帧，而是一个**持有函数对象的槽位**。
DECREF 把「槽位的地址」当成了「对象」，于是把槽里的函数对象当作引用计数减一。**

**`x27` 的计算本身没问题（`sub x27, x24, w19, sxtw #3`，当时 `x19=2`），
错的是放进去的值。**

## 目前的结论

- **故障是一处 `Py_DECREF` 作用在了栈槽地址上，而不是对象上**
- **受影响的字段是帧的 `f_func`**
- **两边无法在指令级长期对齐，因为指针身份比较依赖堆布局**

**要再往前，需要能「在两次运行间比较同一个对象的身份」的手段，
或者把堆分配做成与 qemu 完全一致。前者更现实。**

## 栈上到底是什么

**在失败点把 `x24`（栈指针）附近的每个字都按类型命名：**

```
[x24-4] = tuple
[x24-3] = code        ← DECREF 当作可调用对象的那个，其实是代码对象
[x24-2] = ?           ← 「第一个参数」，值是 0x4000806068（槽地址）
[x24-1] = generator
[x24+0] = dict
```

**这不是普通函数调用的栈形态——`code` + 帧 + `generator` 是**
**恢复生成器（`RETURN_GENERATOR` / `SEND`）时的布局。**

**而 `w20 = 2` 的清理循环按「两个参数」去减引用计数，
于是把**生成器的帧**当成了参数之一。**

**所以问题可能不在「谁写坏了槽」，而在于这条路径上「参数个数」的判断。**

**下一步应该查：这条 `cmp w20,#0` / `cmp w20,#1` 的分派，
在生成器恢复路径上 `w20` 应该是什么值。**

## 附：一条被反复验证的限制

**三次尝试（逐指令轨迹、第 N 次执行、按地址配对）都因为「指针身份比较依赖堆布局」
而无法跨运行对齐。这条限制已经写进文档，避免再走回头路。**

## 对照 CPython 源码：`CALL` 的栈约定

**从 `Python/generated_cases.c.h` 的 `TARGET(CALL)` 抄下来：**

```c
PyObject **args    = (stack_pointer - oparg);
PyObject *callable = stack_pointer[-(1 + oparg)];
PyObject *method   = stack_pointer[-(2 + oparg)];   // 纯调用时为 NULL
int is_meth = method != NULL;
int total_args = oparg;
if (is_meth) { callable = method; args--; total_args++; }
```

**所以 `oparg = 2` 时：**

| 位置 | 含义 | 实测 |
| --- | --- | --- |
| `[x24-4]` | `method`（纯调用须为 NULL）| **tuple** |
| `[x24-3]` | `callable` | code |
| `[x24-2]` | `args[0]` | `0x4000806068`（槽地址）|
| `[x24-1]` | `args[1]` | generator |

## 矛盾就在这里

**`method` 非 NULL（是个 tuple），所以 `is_meth = 1`，
`total_args` 应为 `oparg + 1 = 3`，清理应当减 3 个。**

**但实际清理用的是 2（`w20 = 2`）。**

**说明 `CALL` 执行时的 `stack_pointer` 与清理时的 `x24` **不是同一个值**——
两者之间栈指针移动过，而清理仍按旧值算。**

**这解释了为什么 `args[0]` 会读到「槽地址」：它读的根本不是参数区，
而是紧邻的 `_PyCFrame.current_frame` 字段。**

## 下一步

**要查的是：`CALL` 到清理之间，栈指针为什么移动了。
源码里紧跟着的是 `STACK_SHRINK(oparg + 2)`，
本实现很可能在这条路径上把「收缩」做成了「不收缩」或「收缩了两次」。**

---

# 2026-11-21 晚：CPython 开始执行 Python 代码了

## 两个突破

### 1. `PYTHONHOME` 绕开了 getpath 的死路

**故障从 `0x17a884`（14.98M 条）换成 `0x26281c`（24.04M 条）——
多跑了 60%，而且是**完全不同的**位置。**

```
PYTHONHOME=/usr ./tools/rune …python3.12 -c pass
```

### 2. `LD1R` 未实现，卡住了继续前进

**新故障是「未实现指令」`0x4d40cc01` = `ld1r { v1.2d }, [x0]`。**

**编码规律：`0x0D40C000 | (Q<<30) | (size<<10) | (Rn<<5) | Rt`
（`LD1R`/`LD2R`/`LD3R`/`LD4R` 由 bits 14:12 区分）。**

**实现后 CPython 直接跑到 24.6M 条并**正常退出**——
不再崩溃，而是在执行真正的 Python 代码。**

**已用 guest 程序对照 qemu 验证 `ld1r` 的 16b / 8h / 4s / 2d 四种形式，结果一致。**

## 现在暴露的问题：长字符串被破坏

**CPython 现在能打印路径配置了：**

```
PYTHONHOME = '/usrl/irrpytohntt12'        （正确是 /usr/lib/python3.12）
program name = '/sur//in/ypt//n3.12'      （正确是 /usr/bin/python3.12）
```

**短字符串 `/usr` 显示正常，长字符串从第 5 个字符起开始错乱。**

**`argv[0]` 传入的值本身是对的（`E2_TRACE_ARGV` 确认 `"/usr/bin/python3.12"`），
所以是 CPython 内部处理时损坏的。**

**`ld1r` 已验证不是原因。**

## 下一步

**这个「4 字节分块后开始错乱」的形态，指向某个按 4 字节单位处理的循环。
最可能是字节↔宽字符转换路径上还有一条向量指令没实现对——
`ushll2` 曾经就是这个问题，可能还有同族成员。**

---

# 2026-11-21：M4 达成 —— CPython 完整运行

## 结果

```
$ PYTHONHOME=/usr ./tools/rune .../python3.12 t2.py
version 3.12.13
comp    [0, 1, 4, 9, 16, 25]      列表推导
fib     144                        递归
dict    [('a', 1), ('b', 2)]
fmt     x-42                       字符串格式化
obj     P(5)                       类与 __repr__
caught  ZeroDivisionError          异常处理
sum     5050
```

## 压垮骆驼的三根稻草

**都是**字节重排**类指令，而它们的共同特点是：错了不会崩，只会静默打乱数据。**

### 1. `UZP` 和 `ZIP` 语义对调

| 指令 | 正确 | 之前 |
| --- | --- | --- |
| `UZP1` | 取偶元素后**拼接** | 交错 |
| `ZIP1` | **交错**低半部分 | 拼接 |

**缓冲区两半相同时两种实现结果一样，所以 grep/sed 全过、374 条差分全绿。**

### 2. `UMOV` 只读低 64 位

```c
const uint64_t elem = (cpu->v[rn2][0] >> bit) & mask;   // 永远读 v[0]
```

**`mov x2, v1.d[1]` 返回的其实是 lane 0。这条在 memset/memcpy 的热路径上，
也是我一开始误判 `ZIP1` 的原因——测试打印的是同一个值。**

### 3. `EXT` / `TBL` / `REV64/32/16` 未实现

**同族的其余成员。补上后 CPython 才走完启动。**

## 仍然存在的一个问题

**`-c` 传入的源码字符串在长度 > 8 时会被破坏：**

```
print(1+2*3)  →  printrr+2*3)   →  SyntaxError
```

**而通过**脚本文件**运行完全正常。所以这不在解释器核心，
而在 `-c` 那条字符串处理路径上，长度 8 是分界。**

## 复现方法

```
PYTHONHOME=/usr ./tools/rune /tmp/alpine-root/usr/bin/python3.12 脚本.py
```

**`PYTHONHOME` 绕开 getpath，脚本文件绕开 `-c` 的字符串问题。**

## 完整功能验证（含标准库导入）

```
$ E2_CAP=400000000 PYTHONHOME=/usr E2_HEAP_MB=256 ./tools/rune .../python3.12 t3.py
argv /t3.py
cwd /
json {"k": [1, 2, 3]}                     import json 成功
sorted [1, 2, 3]
join a-b-c
upper ABC
range [0, 1, 2]
bool True True
slice bcd
nested [[0, 0], [0, 1], [1, 0], [1, 1]]
loop 45
lambda 42
```

**`import json` 会递归导入 `re` → `enum` → `functools`，全部走通。**

## 运行命令

```
E2_CAP=400000000 PYTHONHOME=/usr E2_HEAP_MB=256 \
  ./tools/rune /tmp/alpine-root/usr/bin/python3.12 脚本.py
```

**三个环境变量的作用：**

| 变量 | 为什么需要 |
| --- | --- |
| `PYTHONHOME=/usr` | 绕开 getpath 的路径推导 |
| `E2_HEAP_MB=256` | CPython 的分配器需要更大的堆（默认 16 MB 是给 busybox 的）|
| `E2_CAP=400000000` | 导入标准库要 4000 万条指令以上 |

## 堆大小为什么是可配置的

**把默认堆从 16 MB 提到 64 MB 后，一条 busybox 用例从 3 秒变成 40 秒
（`sys 26s`）——每次 fork 都要带上这块堆。**

**所以默认保持 16 MB，需要大堆的场景用 `E2_HEAP_MB` 显式指定。**

**栈同理：`e2_machine_init` 的第三个参数是栈大小，我曾误当成总内存调到 256 MB，
每个进程都分配一次，同样拖慢。已恢复 16 MB。**

## 仍未解决：`-c` 的源码字符串

**`-c 'print(12)'` 会变成 `printrr2)`——长度超过 8 时，第 5、6 个字符
总被替换成源串第 2 个字符。**

**通过脚本文件运行完全正常，所以不影响主要用途，但根因还没找到。**

## `-c` 与环境变量：定位到的规律

**`-c` 的源码和长环境变量值都会在**第 7、8 个字节**被破坏，
替换成源串第 3 个字节：**

```
PYTHONHOME=/abcdefghijklmnop   →  /abcdefgddjklmnop     (len 17)
PYTHONHOME=/abcdefghij        →  /abcdefgh               (len 9，正常)
PYTHONHOME=/abcdefghij        →  /abcdefccij             (len 11)
```

**阈值是长度 ≥ 11；破坏点正好落在 8 字节边界（第 7、8 字节）。**

## 已排除的方向

| 怀疑 | 结论 |
| --- | --- |
| `memcpy`/`strlen` 通用路径 | **排除**——busybox 往返同样的字符串完全正常 |
| `os.fsdecode` 解码路径 | **排除**——长度 8/9/11/13/17/20 全部正确 |
| 我的 harness 传参 | **排除**——guest 收到的是正确字符串 |

**所以问题在 CPython **早期配置解析**用的 `Py_DecodeLocale`
（它走 musl 的 `mbrtowc`，与 `os.fsdecode` 的编解码路径不同）。**

**影响范围很小：脚本文件运行完全正常，只有 `-c` 和超长环境变量受影响。**

