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
