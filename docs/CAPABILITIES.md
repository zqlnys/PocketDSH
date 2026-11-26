# Tier D 能力清单

这份文件说的每一件事都有对应的测试；数字是本地实测的，不是估计。

**最后核对：2026-11-24，版本 0.1.20261124 (915)。**

## 能用

| 能力 | 证据 |
| --- | --- |
| busybox 全套 applet | 384 条差分用例与 qemu 逐字一致（1 条已知差异，见下） |
| CPython 3.12 + 标准库 | 30 个模块的广度探针、13 行窄探针，输出与 qemu 逐字一致 |
| `fork` / `exec` / `wait4` | 一个进程里 fork + wait 100 次；`subprocess` 可运行 |
| 管道、`epoll`、`select` | shell 管道、`select.epoll` 建 12 个、`select.select` 与 `selectors` 与 qemu 一致 |
| 真实休眠 | `time.sleep(0.25)` 真的等 250 ms，与 qemu 一致 |
| 取消正在跑的命令 | 每 65536 条指令检查一次，300 ms 内停下 |
| 写入上限 | 运行期 64 MB `RLIMIT_FSIZE`，超出时写失败 |
| 内存上限 | `E2_HEAP_MB` 同时约束 `brk` 与 `mmap`，超出抛 `MemoryError` |
| 工作区挂载 | guest 的 `/workspace` 就是项目目录 |
| **27 个系统调用** | `tools/syscall-probe.py` 逐行与 qemu 对比：`getpid`、`stat`、`symlink`、`hardlink`、`chmod`、`listdir`、`walk`、`pipe`、`select`、`lseek`、`getcwd`、`time` 等 |

## 这些模块可导入

```
ssl  hashlib  bz2  lzma  sqlite3  pyexpat  decimal
xml.etree.ElementTree  curses  curses.panel
```

**`gdbm` 不行**——缺的是 Python 侧的模块（裁剪时去掉了），不是共享库。构建脚本会报告这件事，不会静默跳过。

## 已知错误

| 症状 | 影响 |
| --- | --- |
| **`pow()`、`math.hypot()`、`math.log2()` 结果错误** | 误差是常数 `+0.0223674`。**十轮的定位结论**：`log2` 确实在执行；归约结果 `d7 = 1.0` 精确；`fmov d2, #-1.0` 精确；`ubfx` 的索引（40）与编码一致；从 `.rodata` 读到的表项 `0.9922480467550775` 与 E2 加载的值**完全一致**——也就是说 **E2 的内存读取本身没错**，而 `1.0` 出现在表内另一个位置（`0x9B540`，对应索引 73，超出 6 位字段范围）。**结论：我对这张表布局的理解是错的，问题不在已排除的那些指令上。** 已排除：加减乘除、比较、`sqrt`、`exp2`、`log`、`log10`、`log1p`、`exp`、`frexp`、`ldexp`、`floor`、`trunc`、`int()`、双↔单精度转换、`ubfx`、64 位逻辑立即数、`fmov` 立即数与寄存器形式、`fmadd` |

## 已知错误

| 症状 | 影响 |
| --- | --- |
| **`LANG=C` 且没有 `LC_ALL` 时，`/workspace` 下的路径被破坏** | `python3.12 /workspace/t.py` 报 `can't open file '/wor'`。**范围已收窄**：`/workspace` 本身、`/workspac`、`/abcdefghij`、`/a/b/c/d/e` 全部正常，**只有以 `/workspace/` 开头、因而走工作区映射分支的路径会被破坏**。同样的命令在 `LANG=C.UTF-8` 下正常。**已缓解**：`LC_ALL` 现在真的会传到 guest（此前桥接层设了它、harness 没有透传，等于空操作）。**也未发现**：harness 传给 guest 的 argv 是完整的（`[argv] 1 = "/workspace/t.py"`），所以破坏发生在 guest 内部；`mbrtowc` 在 C locale 下经 ctypes 实测完全正确。根因未找到 |
| **`pow()`、`math.hypot()`、`math.log2()` 结果错误** |
| 限制 | 原因 |
| --- | --- |
| **没有网络** | 只支持 `socketpair`，没有任何会 `connect` 的东西。**`ssl` 能导入、CA 证书齐全，但发不出 HTTPS 请求。** |
| **`socket.socketpair` 不可用** | 系统调用本身有，但 Python 的包装还需要 `sendmsg`/`recvmsg` 传描述符，两者返回 ENOSYS |
| **没有线程** | 见下 |
| **没有 `/proc/mounts` 一致性** | `df` 的 `st_dev` 与 `/proc/mounts` 对不上，`df` 输出不可信 |
| **`prlimit64` 只报告不执行** | 设置新的 rlimit 会被接受但忽略 |

## 关于线程

`clone` 能创建执行流，但跨执行流的 pc 会损坏——故障转储里出现过 `pc=0x872ca4`，即 `0x400000872ca4` 的高 32 位丢失，且不在任何映射里。

**这一点试过十一轮，其中四次得出了自己造成的错误结论**（继承的指令计数、错误的采样点、逐指令的区域扫描、bash 的 `sigsuspend`）。**目前关闭，记录在 `docs/persistent-session.md`。**

**CPython 的 `threading` 依赖它，所以 `threading` 不可用。**

## 性能

```
合成循环       50 ns/指令
真实负载       72 ns/指令（CPython 启动，3910 万条指令）
```

**分发不是瓶颈**：Python 负载上每条指令只有 2.0 次守卫比较（加前门判别前是 33.6 次）。剩下的差距是缓存行为。

**真实负载那个数才是该盯的**——合成循环测的是分发，而分发早就不是成本所在。

## 已知差异

差分对比里有一条长期存在的已知差异，记录在 `tools/diff-test.sh` 的注释里；其余 384 条与 qemu 完全一致。

## 怎么自己验证

```sh
cd e2
make test                                    # 334 条指令级检查
./tools/diff-test.sh <busybox.static> ./tools/rune   # 384 条与 qemu 对比
./tools/fs-test.sh                           # 68 条端到端
./tools/perf-check.sh                        # 两个性能数字
```

iOS 侧：

```sh
cd ..
./scripts/preflight.sh                       # 预检
# CI 会跑 85 条设备测试
```
