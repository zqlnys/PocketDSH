# Tier D 能力清单

这份文件说的每一件事都有对应的测试；数字是本地实测的，不是估计。

**最后核对：2026-11-28，版本 0.1.20261128 (1060)。**

## 这些证据是在哪测的

**105 条 iOS 测试跑在 iOS 模拟器上（Apple Silicon 的 CI runner），跑的是同一份源码、为真机构建。**

**它证明了什么：**解释器、musl 加载器、系统调用层和文件系统在 Darwin 上、在 app 沙盒里、以 arm64 真正执行了，而不只是编译通过。

**它没有证明什么：**真机上的行为。模拟器和真机共用内核，但内存压力、看门狗和 JIT 限制的表观不同——**E2 不需要 JIT，也不需要 `fork()`，这两条限制对它不成立；但真机上还没跑过。**

**要补上这一块，**需要在有签名的真机上跑同一套测试。这里没有签名证书，所以这条限制是真的，不是疏忽。

## 能用

| 能力 | 证据 |
| --- | --- |
| busybox 全套 applet | 385 条差分用例与 qemu 逐字一致（1 条已知差异，见下） |
| CPython 3.12 + 标准库 | 30 个模块的广度探针、13 行窄探针，输出与 qemu 逐字一致 |
| `fork` / `exec` / `wait4` | 一个进程里 fork + wait 100 次；`subprocess` 可运行；失败的 `exec` 返回 `ENOENT`，过长的参数返回 `E2BIG` |
| 管道、`epoll`、`select` | shell 管道、`select.epoll` 建 12 个、`select.select` 与 `selectors` 与 qemu 一致 |
| 真实休眠 | `time.sleep(0.25)` 真的等 250 ms，与 qemu 一致 |
| 取消正在跑的命令 | 每 65536 条指令检查一次，300 ms 内停下 |
| 写入上限 | 运行期 64 MB `RLIMIT_FSIZE`，超出时写失败 |
| 内存上限 | `E2_HEAP_MB` 同时约束 `brk` 与 `mmap`，超出抛 `MemoryError` |
| 工作区挂载 | guest 的 `/workspace` 就是项目目录 |
| **持久会话** | `E2_SESSION=1` 时一个 guest shell 跨多条命令存活，`cd` 和变量都保持。命令由写线程分次喂入，shell 阻塞在 read 上等着——不是轮询。完成标记走 `E2_SESSION_MARK` 指向的独立文件，输出里没有分帧痕迹 |
| **`asyncio` 事件循环** | `sleep`、`gather`、`Queue`、`run_until_complete` 都可用——它需要的 socketpair 自管道现在能收发。**TCP 服务端不行**，因为没有东西能 `connect`（见「没有网络」） |
| **`time.gmtime` / `strftime`**（曾因带移位加减法里 `ASR` 写成逻辑右移而错，已修复） | 与 qemu 逐字段一致，包括 `tm_wday`。文件系统套件跑六个数并直接与 qemu 的输出比对 |
| **`/proc` 已种入** | `df`、`free`、`uname -a`、`nproc` 都能用；`/proc/mounts` 与 `statfs` 一致；`/proc` 与 `/proc/self` 都从可写目录提供，因为设备上的 rootfs 只读 |
| 深嵌套的子 shell | 1000 层括号嵌套仍能运行 |
| **27 个系统调用** | `tools/syscall-probe.py` 逐行与 qemu 对比：`getpid`、`stat`、`symlink`、`hardlink`、`chmod`、`listdir`、`walk`、`pipe`、`select`、`lseek`、`getcwd`、`time` 等 |

## 这些模块可导入

```
ssl  hashlib  bz2  lzma  sqlite3  pyexpat  decimal
xml.etree.ElementTree  curses  curses.panel
```

**`gdbm` 不行**——缺的是 Python 侧的模块（裁剪时去掉了），不是共享库。构建脚本会报告这件事，不会静默跳过。

**`PYTHONPYCACHEPREFIX` 故意不传给 guest**：指向可写前缀会让 Python 重新编译标准库并写出结果，而包里已有的预编译字节码反而不用。实测 `import asyncio` 是 155 秒对 24 秒。

## 已知错误

这张表只列**确认存在、尚未修复**的问题，每条带上已经排除的东西——**下一轮不必从同样的地方开始。**

| 症状 | 已经确定和已经排除的 |
| --- | --- |
| **`awk` 的 `"cmd" \| getline` 和 `system()` 不工作** | 两者都经 `popen`，它要 `clone(CLONE_VM\|CLONE_VFORK\|SIGCHLD, stack)`。非 NULL 的栈让这条路径被当作线程，返回 `EINVAL`，于是 `system()` 返回 -1、`getline` 拿到空串。**`vfork`（58）已实现为 fork，但 busybox 走的是 clone。试过把 `CLONE_VM\|CLONE_VFORK` 当 fork 处理，进程在内核态死掉，比原来更糟，已撤回。** |
| **`math.hypot()` 两个及以上参数返回 `nan`** | `libm.hypot` 经 ctypes 直接调用返回 5.0，**所以 musl 没坏**；`math.so` 的导入表里没有 `hypot`，**所以 `math.hypot` 从不调它**，走的是 CPython 自己的 `vector_norm`。分支已被区分开：`hypot(0,0)`、`hypot(inf,1)`、`hypot(nan,1)` 三条提前返回**全对**，只有有限值的累加路径错——而且 `hypot(3.0, 0.0)` 也错，所以只要**有第二个元素**就进那条路。`nan` 最可能来自 `sqrt` 收到负参数。**已排除**：`frexp`、`ldexp`（含负指数）、`sqrt`、`fabs`、`CCMP`、`FCMP`、以及 `sqrt` 之前那三十条 Dekker 累加指令（在 E2 里精确算出 25.0）。`math.so` 与 `libpython` 都无符号可读。 |
| **`math.log2()` 差一个固定常数，`pow()` 因而全错** | `log2(2.0)` 得 `1.0223674090047083`，**恒差 `δ = 0.0223674090047083`**，`log2(1.0)` 精确。musl 的 `pow` 走它，所以 `pow(2,2)` 得 `-1.537…`。**`log`、`log10`、`exp`、`exp2`、`log1p` 都正确。** 已排除：Python 层算术、`SMULH`/`UMULH`、C 层除法、`TBZ`/`TBNZ`、`fmov` 位模式传递、64 位逻辑立即数、`movz`/`movk`、**以及范围归约本身**（用库里真实编码逐条验证：每个 2 的幂都精确归约到 1.0、表索引都是 0x31）。表项 `0x9b330 = 0.9922480467550775` 已读出，多项式在 `-0.00775` 上求值却返回正值。 |
| **`datetime.datetime` 的前三个字段恒为零** | 位置参数也一样。`datetime(2021,7,15,5,6,7)` 得到年 0、月 0、日 0，而时、分、秒正确；`datetime.date(2021,7,15)` **全对**。**已排除**：调用约定（四种形式）、`datetime.py` 包装（C 层 `__new__` 同样为零）、关键字解析器、参数是否到达（类型校验正常）、超过八个参数的调用（`snprintf` 十一个参数正确）、getter/repr（`__reduce__` 显示 `data[]` 前四字节为零）。`now()`、`strptime()`、`combine()`、`fromordinal()` 全部受影响。 |

| 限制 | 原因 |
| --- | --- |
| **没有网络** | 只支持 `socketpair`，没有任何会 `connect` 的东西。**`ssl` 能导入、CA 证书齐全，但发不出 HTTPS 请求。** |
| **`socket.socketpair` 可以收发数据** | `sendto`(206)、`recvfrom`(207)、`sendmsg`(211)、`recvmsg`(212) 都已实现，往返数据通过。**`SCM_RIGHTS`（传描述符）仍不支持，带控制消息的调用返回 `ENOTSUP`——明说而不是悄悄丢掉** |
| **没有线程** | 见下 |
| **`run_linux` 还没有用上会话模式** | 会话在 harness 和桥接两层都已工作（`e2_bridge_session_start/_write/_wait/_close`），但 `run_linux` 仍每次启动新 guest。把它接上是下一步 |


## 关于线程

`clone` 能创建执行流，但跨执行流的 pc 会损坏——故障转储里出现过 `pc=0x872ca4`，即 `0x400000872ca4` 的高 32 位丢失，且不在任何映射里。

**这一点试过十一轮，其中四次得出了自己造成的错误结论**（继承的指令计数、错误的采样点、逐指令的区域扫描、bash 的 `sigsuspend`）。**目前关闭，记录在 `docs/persistent-session.md`。**

**CPython 的 `threading` 依赖它，所以 `threading` 不可用。**

## 性能

**构建机上（`-O2`，`perf-check`）：**

```
合成循环       49–52 ns/指令
真实负载       74–77 ns/指令（CPython 启动，3910 万条指令）
```

**设备上（Release，iOS 模拟器，实测单测耗时）：**

```
import asyncio          25.4 秒   （-Os 构建时是 68.0 秒）
python3 -c 'pass' 级别   3.2–7.2 秒
```

**这两个是不同的问题，之前混在一起：构建机上的数字一直是 `-O2`，而设备上跑的是 `-Os`，差 2.8 倍。现在两处都是 `-O2`，CI 会守住它。**

**分发不是瓶颈**：Python 负载上每条指令只有 2.0 次守卫比较（加前门判别前是 33.6 次）。剩下的差距是缓存行为。

**真实负载那个数才是该盯的**——合成循环测的是分发，而分发早就不是成本所在。

**测量的噪声底：单次采样 75–81 ns（约 8%）；`perf-check` 现在跑五次取最小值，降到 74–77 ns（约 4%）。**比这更小的改动用这个脚本分辨不出来——本次会话有三次优化落在这个区间内，其中包括一个四项区域缓存（读数 75–76 对 73，已撤回）。**指令数是确定的，所以多次运行之间的差异是机器的，取最小值把这个差异压掉。**

**已经测过并否决的方向**（都在 4% 分辨率内，或没有效果）：

| 方向 | 结果 |
| --- | --- |
| 四项区域缓存 | 75–76 对 73，在噪声内 |
| 区域查找改二分 | 12 次采集中位数约 75，与线性扫描相同 |
| `-O3`、`-fno-plt` | 与 `-O2` 完全相同 |
| `PYTHONPYCACHEPREFIX` 透传 | 24 秒 → 155 秒，反而更慢 |

**二分那条说明单项命中缓存已经让查找足够罕见——查找不是成本所在。**

**真正的那个数量级来自别处：`-Os` → `-O2` 是 2.8 倍（68 秒 → 24 秒）。**在这之前，所有微优化都在一个被 2.8 倍掩盖的区间里测量。**

**剩下的差距是结构性的**：3910 万条指令、74–77 ns/条，而原生只要约 30 ms。**要跨过它需要译码缓存或线程化分发这一类改动，不是改一行。**

## 已知差异

差分对比里有一条长期存在的已知差异，记录在 `tools/diff-test.sh` 的注释里；其余 384 条与 qemu 完全一致。

## 怎么自己验证

```sh
cd e2
make test                                    # 412 条指令级检查
./tools/diff-test.sh <busybox.static> ./tools/rune   # 384 条与 qemu 对比
./tools/fs-test.sh                           # 95 条端到端
./tools/perf-check.sh                        # 两个性能数字
```

iOS 侧：

```sh
cd ..
./scripts/preflight.sh                       # 预检
# CI 会跑 105 条模拟器测试
```
