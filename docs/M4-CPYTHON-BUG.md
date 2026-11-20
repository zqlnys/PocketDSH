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

