# E2 后端：许可自由的 Linux 运行时

状态：**M1 完成**（解释器 + ELF 加载 + syscall，能跑真实 aarch64 程序）
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
| 指令解码 | MOVZ/MOVN/MOVK、ADR/ADRP、ADD/SUB（immediate + shifted register）、AND/ORR/EOR/ANDS、B/BL、B.cond、CBZ/CBNZ、BR/BLR/RET、LDP/STP（offset/pre/post）、LDR/STR（unsigned offset）、SVC、NOP |
| 寄存器 | X0–X30 + SP 分离，NZCV 正确建模 |
| 内存 | 平坦 48 位地址空间，区域管理 + 权限位 + `mprotect` |
| ELF | 静态 ELF64 aarch64，多段、页对齐处理、加载期可写后降权限；动态可执行明确拒绝 |
| syscall | `write`、`writev`、`exit`、`exit_group`；未知调用返回 `-ENOSYS` |
| 测试 | 34 项，含两个交叉编译的真实 ELF（hello world、递归 fib） |

### 已验证的端到端用例

- `hello.elf`：`mov` + `adr` + `svc write` + `svc exit`，输出逐字节校验
- `fib.elf`：递归 fib(10)=55，含 STP/LDP 栈帧、BL/RET、`b.lt`、栈上 spill 槽，1956 条指令

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
