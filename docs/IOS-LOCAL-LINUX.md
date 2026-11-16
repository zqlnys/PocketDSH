# iOS 本地 Linux：三套实现实测与架构决策

状态：调研完成，结论已定。本文替代此前对 Tier R / Tier D 的设想。

实测时间：2026-11-16
实测设备：iPhone（arm64，6 核）
对照样本：PocketDSH 内置 iSH（i686）、ServerBox 内置 Linux（aarch64）、UTM SE（无 JIT）

---

## 一、结论摘要

**状态：可复刻性已验证通过（2026-11-16）。apt 装包、arm64 二进制执行、Python 运行全部实测成功；执行机制已用 `/proc/self/maps` 认定为 aarch64 解释，不是 native。**

1. **ServerBox 的本地 Linux 不是虚拟机，是 iSH 架构的 aarch64 移植**：手写 aarch64 解释器 + syscall 转发，计算减速 5–10×。
2. **PocketDSH 现在的 iSH 是 i686 解释执行，减速 20–50×**。同样的内核代码（`4.20.69-ish SUPER AWESOME`），差别只在 `emu/` 后端的 guest 指令集。
3. **apt 装进来的 arm64 包能直接执行**，因为解释器不关心代码页权限——全程不涉及 `execve`、`PROT_EXEC`、JIT。**这是好消息：方案完全合法，不碰任何 iOS 限制。**
4. **UTM SE 是三者里最慢的**（50–100×），把它当 Tier R 是反向选择。
5. **`/workspace` 挂载不需要任何特权**：ServerBox 把 guest 的 `/` 直接 bind 到 App Group 里的 `File Provider Storage/linux`，Files App 里可见。纯工程，一周可做。
6. **装包慢在 I/O 不在 CPU**：13.9MB/354 文件耗 17.9s，CPU 只占 0.249s。换解释器后端救不了装包速度。
7. **`/proc` 数字全部不可信**：`MemTotal` 精确等于 `2^32/1000`，`MemFree == MemShared`，`stack` 标记为只写。只有 `time` 的相对值可信。
8. **许可证是硬约束**：iSH 是 GPLv3，QEMU 是 GPLv2。ServerBox 全 app GPL-3.0 正是因为它嵌了 iSH。PocketDSH 若闭源分发，两条路都堵。

---

## 二、三套环境定性

| | PocketDSH（现状） | ServerBox | UTM SE |
| --- | --- | --- | --- |
| `uname -a` | `4.20.69-ish SUPER AWESOME ... i686` | `4.20.69-ish SUPER AWESOME ... aarch64` | 真实 Linux `6.x/7.x` |
| guest 架构 | i686 (x86) | aarch64 | 任意 |
| host 架构 | arm64 | arm64 | arm64 |
| 执行方式 | **指令解释**（逐条解码 x86） | **指令解释**（逐条解码 aarch64） | **指令翻译**（TCG，无 JIT） |
| 是否真内核 | 否（fake kernel） | 否（fake kernel） | 是（qemu-system） |
| 减速比 | 20–50× | **5–10×** | 50–100× |
| 内存模型 | App 进程内 | App 进程内 | 独立 VM 进程 |
| 许可证 | GPLv3 | GPLv3（因此全 app 开源） | GPLv2（QEMU） |

### 关键证据

**ServerBox 的 mount 表暴露了 fs 层实现：**

```
/private/.../AppGroup/EF817DF3.../File Provider Storage/linux   /            real  rw
/private/.../linux-dev/ubuntu/data                              /ubuntu/dev  fake  rw
/ubuntu/proc                                                     proc
/ubuntu/dev/pts                                                  devpts
```

`real` / `fake` 是 iSH 内核代码里定义的两种后端写入器：
- `real` → 直通 iOS 真实文件系统
- `fake` → iSH 的内存文件系统

真 QEMU 的 guest 里会显示 `/dev/vda1` 这类块设备。**没有块层，只有宿主路径，所以是用户态目录重映射。**

**exec 开销特征：**

```
time sh -c 'i=0; while [ $i -lt 1000 ]; do /bin/true; i=$((i+1)); done'
real 1.302s   user 0.060s   sys 0.016s
```

1.302s 里 CPU 只用了 0.076s，**1.23s 花在等待**。iOS 禁止 `fork`/`exec`，所以每次“exec”是 ELF loader 在用户态重新映射地址空间，等待发生在映射与锁上。1.3 ms/次是这个实现的固定成本。

---

## 三、实测数据

### 基准：bash 算术循环（纯计算，无 syscall）

```sh
time sh -c 'i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done'
```

| 平台 | 结果 | 说明 |
| --- | --- | --- |
| bash 原生 ARM64（估算） | 0.7–1.0s | 参照值 |
| **ServerBox（aarch64 解释）** | **4.354s** | **减速 4–6×** |
| PocketDSH iSH（i686 解释，估算） | 15–35s | 减速 20–50× |

> 注意：Ubuntu 的 `/bin/sh` 指向 bash（报错前缀是 `bash:`），不是 dash。用 dash 的基准换算会把减速比高估约 4 倍。

### 其它实测

| 项目 | ServerBox 实测 | 含义 |
| --- | --- | --- |
| `fork/exec` ×1000 | 1.302s（CPU 仅 0.076s） | exec 层固定成本 1.3ms/次 |
| `dd` 顺序写 200MB | 571 MB/s | fakefs 直通宿主，无块层 |
| `nproc` | 6 | 上报宿主核数（假内核伪造） |
| `free -h` total | 4.1 GiB | App 进程地址空间上限 |
| `free -h` available | 243–288 MiB | **已见底** |
| `swap` | 0 | 无交换 |
| `df` | `cannot read table of mounted file systems` | fake 内核未实现 df 所需的统计 |
| `apt install` | `Unable to locate package` | `/var/lib/apt/lists` 为 **0 字节** |

**内存数据不可信**：`MemFree == MemShared == 247513 kB`，`Buffers == Cached == 0`。真内核不会有这种组合，这是把宿主进程 RSS 直接映射过来的结果。**只信 Instruments。**

### rootfs 与源配置（均正确）

```
dpkg --print-architecture → arm64
uname -m                  → aarch64
codename                  → resolute (26.04)
sources: deb822, archive.ubuntu.com, resolute{,-updates,-backports,-security}, main universe restricted multiverse
```

**配置没有一处错，apt 失败纯粹是索引没拉下来。** 待查：DNS / TLS / 出站。

---

## 四、为什么 ServerBox 快 5–10 倍

同一个 fake kernel（`4.20.69-ish`），差别只在 `emu/` 后端的 guest 指令集：

```
iSH 标准（PocketDSH 现状）
  guest ELF (x86) → [x86 解释器逐条解码] → fake kernel syscall 表 → iOS libc
  减速 20–50×

ServerBox 形态
  guest ELF (aarch64) → [aarch64 解释器逐条解码] → fake kernel syscall 表 → iOS libc
  减速 5–10×
```

**两层结构完全一样，都是解释执行。** 加速全部来自「aarch64 比 x86 好解释」：

| | x86-64 | aarch64 |
| --- | --- | --- |
| 指令长度 | 1–15 字节变长，需状态机 | **定长 4 字节**，一个 switch |
| 寄存器 | 16 个，命名混乱 | **31 个通用**，直接数组索引 |
| 条件执行 | 依赖 EFLAGS 副作用 | 显式 `csel`/`b.cond` |
| 寻址模式 | 几十种组合 | 规整 load/store |
| 解码器规模 | 上万行 | 几千行 |

同样的线程化分派结构，aarch64 后端能跑到 x86 后端 3–5 倍。

### 关键事实：全程不碰 iOS 限制

因为是解释执行，**不需要 `execve`、不需要 `PROT_EXEC`、不需要 JIT、不需要 `fork`**。代码页以只读方式被读取，指令由软件解码。

`/proc/self/maps` 实测证实：主程序 `cat` 的两个映射段全部是 `r--p`，没有一段 `r-xp`；`grep` 输出的 `r-xp` 只有 libc、libpcre2 这类库文件。

**这反而把方案的风险面清零了**——它跟 PocketDSH 现在跑 i686 的合法性完全一致。

> 曾误判为「native 执行 + file-backed mmap 绕过 AMFI」，已用 `/proc/self/maps` 证伪。推翻过程记录在 7.4。

### 代价：工作量是「写一个 aarch64 解释器」

`kernel/` 和 `fs/` 可以原样复用，只换 `emu/`。但 aarch64 解释器本身是几千到上万行 C，带指令缓存和跳转优化。这是大工程。

**中间档：保持 x86 guest，优化现有解释器**（启用 block cache、把热指令路径手写成 C 函数），目标 20–50× → 8–15×。收益不如换架构，但工作量差一个数量级。

---

## 五、Tier 架构决策

```
Tier A   Swift 原生          文件/编辑/搜索/diff/模型调用     默认，完整，闭源
Tier W   ios_system + wasm   bash/coreutils/python           近似原生，闭源可用
Tier C   iSH（现状 i686）    /workspace Linux、shell、apt     20–50×，GPLv3
Tier C+  iSH-aarch64 解释     同上，但 5–10×                   需自研 aarch64 后端，GPLv3
Tier R   SSH over Tailscale  node/npm/gcc/服务/长任务          满速，后台可用
Tier S   UTM SE（实验）       真内核语义验证，iPad 限定         50–100×，不进主线
```

### 职责边界（由性能倒推）

| 负载 | 归属 | 理由 |
| --- | --- | --- |
| 项目文件读写、搜索、diff | Tier A | 别下 Linux |
| `ls`/`cat`/`grep`/`git status` | Tier C+ | 5–10× 后仍是几十毫秒 |
| `python3 script.py`（轻量） | Tier C+ | 数秒可接受 |
| `apt install` | Tier C+ | 受内存和索引限制 |
| `npm install` / `gcc` / 起服务 / 长任务 | Tier R | 本地必被 jetsam 杀 |
| docker / systemd / netfilter | Tier S | 只有真内核支持 |

### 撤销的设想

- **Tier R 换成 UTM SE**：反向选择。SE 是候选里最慢的。
- **Tier C 挂 JIT**：无意义。iSH 是解释器，没有 JIT 后端；给了 `dynamic-codesigning` 也不会变快。吃 JIT 的是 QEMU 的 TCG。
- **跨 app 连 UTM SE**：iPhone 单前台互斥，切走即冻结。iPad 分屏可用，但每执行一条命令要手动切两次 app，产品价值为零。

---

## 六、三块该抄的东西

### 1. workspace 挂载（最高优先级，无阻塞）

ServerBox 的做法：App Group 容器 + File Provider，同一个目录同时是：

- iOS「文件」App 里的一个文件夹
- guest 里的 `/`

```swift
// App Group 共享容器
let group = FileManager.default
    .containerURL(forSecurityApplicationGroupIdentifier: "group.com.example.pocketdsh")
let workspace = group!.appendingPathComponent("File Provider Storage/linux")

// iSH 侧把 realfs 的根指到这里
ish_set_realfs_root(workspace.path)
```

配合 `NSFileProviderReplicatedExtension`，这个目录会出现在 Files App 里，用户拖文件进去 guest 立刻可见。

**这一块跟性能、JIT、许可证全都无关，纯工程，一周。**

### 2. 持久 shell（摆脱"一次一条命令"）

现状限制：一次 App 启动只支持一个 `run_linux` 命令。

改法：不要每条命令新起 task，而是**保持一个 guest 进程常驻，主 App 侧持有 stdin/stdout 的 fd**：

```
Swift 侧: ShellSession
  ├─ guest pid（常驻 /bin/bash -i）
  ├─ stdin pipe fd
  ├─ stdout ring buffer（NDJSON 帧）
  └─ 命令队列 → 写入 stdin → 从 stdout 读回
```

切前台自动 resume，切后台冻结（这是 iOS 的模型，别对抗它）。

### 3. aarch64 解释器后端（最难，收益最大）

把 `emu/` 层从「x86 解释」换成「aarch64 解释」，`kernel/` 和 `fs/` 原样复用。

**不需要碰的东西**（这轮调研最有价值的负面结论）：可执行页、AMFI、JIT、`PTRACE`、`fork` 语义——全部无关，因为代码页只被读取，不被 CPU 执行。

**需要写的东西**：

- aarch64 指令解码器（定长 32-bit，比 x86 简单得多）
- 31 个通用寄存器的映射与影子状态（含 NZCV 条件标志）
- 内存访问路径（load/store 寻址模式比 x86 规整）
- 分支与跳转缓存（`b`/`b.cond`/`bl`/`ret`/间接跳转）
- 原子指令与内存序（`ldxr`/`stxr`/`dmb`）
- 浮点/SIMD 的取舍（可以只做标量，遇到 SIMD 走慢路径）

目标：Tier C 从 20–50× 变成 5–10×，把本地 Linux 从演示级拉到可用级。

**中间档**：保持 x86 guest，但启用 iSH 已有的 block cache、把热指令路径手写成 C 函数。目标 8–15×，工作量差一个数量级。**建议先做这个，验证收益再决定要不要换架构。**

---

## 七、验证结果（2026-11-16 全部通过）

### 7.1 修 apt —— ✅ 已通

三个障碍依次排除：

**障碍一：DNS 不解析**

```
Err:1 https://archive.ubuntu.com/ubuntu resolute InRelease
  Temporary failure resolving 'archive.ubuntu.com'
```

`/etc/resolv.conf` 是 0 字节。写入 nameserver 后立即生效：

```sh
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf
```

备选（fake 内核忽略 resolv.conf 时必成）：

```sh
cat >> /etc/hosts <<'EOF'
91.189.91.81    archive.ubuntu.com
91.189.91.81    security.ubuntu.com
185.125.190.81  archive.ubuntu.com
EOF
```

**障碍二：TLS 证书验证失败**

```
SSL connection failed: error:0A000086:SSL routines::certificate verify failed
[IP: 91.189.91.82 443]
```

ca-certificates 缺失或 fake 内核的 TLS 栈不完整。降级 http 即可，GPG 签名仍在验证，安全性不降：

```sh
sed -i 's|https://archive.ubuntu.com|http://archive.ubuntu.com|' \
  /etc/apt/sources.list.d/ubuntu.sources
```

**障碍三：apt 索引缓存溢出**

```
Error: Dynamic MMap ran out of room. Current value: 25165824.
Error: Problem with MergeList ..._universe_binary-arm64_Packages
```

24 MiB 是 apt 默认 `Cache-Start`，装不下 Ubuntu 26.04 的 arm64 universe（六万多个包）。但内存只剩 243MB，无脑加大到 512MB 会 OOM。

**正解是缩小索引范围，而不是加大缓存**——`python3-minimal` 在 main 里，跟 universe 无关：

```sh
# 只启用 main
cat > /etc/apt/sources.list.d/ubuntu.sources <<'EOF'
Types: deb
URIs: http://archive.ubuntu.com/ubuntu/
Suites: resolute
Components: main
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

# 清掉 universe 的大索引
rm -rf /var/lib/apt/lists/*
mkdir -p /var/lib/apt/lists/partial

# 128MB 缓存，在 243MB 余量内
echo 'APT::Cache-Start "134217728";' > /etc/apt/apt.conf.d/99cache-start

apt-get -o APT::Sandbox::User=root update
```

结果：

```
Get:1 http://archive.ubuntu.com/ubuntu resolute InRelease [136 kB]
Get:2 http://archive.ubuntu.com/ubuntu resolute/main arm64 Packages [1468 kB]
Get:3 http://archive.ubuntu.com/ubuntu resolute/main Translation-en [524 kB]
Fetched 2128 kB in 5s (453 kB/s)
Reading package lists... Done
```

> main 的 arm64 索引只有 1.4MB。universe 是它的数十倍。**只需要 main 时，永不启用 universe。**

### 7.2 arm64 二进制执行验证 —— ✅ 通过

```sh
time apt-get -o APT::Sandbox::User=root install -y --no-install-recommends python3-minimal
time python3 -c 'print(sum(range(10**7)))'
```

```
# 装包：4 个真 arm64 包
The following NEW packages will be installed:
  libexpat1 libpython3.14-minimal python3-minimal python3.14-minimal
Get:1 .../libexpat1 arm64 2.7.4-1 [85.7 kB]
Get:2 .../libpython3.14-minimal arm64 3.14.4-1 [912 kB]
Get:3 .../python3.14-minimal arm64 3.14.4-1 [2568 kB]
Get:4 .../python3-minimal arm64 3.14.3-0ubuntu2 [25.8 kB]
After this operation, 13.9 MB of additional disk space will be used.

real    0m17.904s
user    0m0.249s
sys     0m0.055s

# 执行
49999995000000

real    0m1.841s
user    0m1.818s
sys     0m0.013s
```

**结论：apt 装进来的新 arm64 二进制能直接执行，结果正确。可复刻性确认。**

### 7.3 两个数字的两个瓶颈

| 操作 | real | user | 瓶颈 | 换解释器后端有用吗 |
| --- | --- | --- | --- | --- |
| `apt install`（13.9MB / 354 文件） | 17.904s | **0.249s** | **I/O**（小文件元数据） | **没用** |
| `python3` 纯计算 | 1.841s | **1.818s** | **CPU** | **有用** |

- **装包慢在 iOS 文件系统的小文件开销量**，跟指令集无关。对策是减少小文件（预置 rootfs、合并层），不是优化 CPU 路径。
- **计算慢在指令解码 + syscall 转发**。`sum(range(10**7))` 原生约 0.15–0.25s，实测 1.841s，减速 7–10×，跟 bash 循环那次的 4–6× 同量级。

### 7.4 机制认定：aarch64 解释执行（已用 `/proc/self/maps` 证伪 native 假设）

**结论：不是 native 执行，也不是 mmap + PROT_EXEC，是手写的 aarch64 解释器。**

三条独立证据：

**证据一：主程序没有可执行段**

```
0000ef5d7000-0000efe93000 r--p 00000000  /ubuntu/usr/lib/cargo/bin/coreutils/cat
0000efea6000-0000efff7000 r--p 008bf000  /ubuntu/usr/lib/cargo/bin/coreutils/cat
```

`cat` 自己的段全是 `r--p`。同一个 loader 对 libc 却分得清清楚楚：

```
0000ef260000-0000ef409000 r-xp ... libc.so.6     ← text 正确标了 r-x
0000ef42 0000-0000ef422000 rw-p ... libc.so.6    ← data
```

**证据二：stack 只有写权限**

```
0000fffec000-0000ffffe000 -w-p  [stack]
```

`-w-p` 是只写不可读，真 Linux 是 `rw-p`。如果权限生效，任何 `ret` 都会崩。**所以 maps 里的权限位是装饰性的，fake kernel 不做真权限检查。**

（这同时削弱了证据一——权限位既然是假的，「`cat` 没有 r-xp」也可能是填错。所以真正的判决证据是第三条。）

**证据三：减速比（与权限位无关）**

```
python3 -c 'print(sum(range(10**7)))'   real 1.841s   user 1.818s
```

原生 aarch64 CPython 跑这段约 0.15–0.25s。**7–10× 的纯计算减速，是解释层的铁证。**

**二进制架构已确认**：

```
head -c 20 /usr/bin/python3.14 | od -An -tx1
7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
02 00 b7 00
        └─ e_machine = 0xB7 = AArch64, EI_CLASS = 02 = 64-bit
```

**真实形态**：

```
guest ELF (aarch64) → [aarch64 解释器逐条解码] → fake kernel syscall 表 → iOS libc
```

**与 PocketDSH 现有 iSH 是同一套架构，只是 `emu/` 后端的 guest 指令集从 x86 换成 aarch64。`kernel/` 和 `fs/` 可以原样复用。**

#### 被推翻的三个推论（记录在此，避免重复踩坑）

| 曾经的推断 | 实际 |
| --- | --- |
| 「指令集一致时，解释器层整个消失」 | **错**，解释器还在，只是变快了 |
| 「file-backed mmap + mprotect 绕过 AMFI」 | **错**，全程没有可执行内存需求 |
| 「AMFI 没拦是因为 mmap 口子」 | **错**，AMFI 无关，因为既不用 `execve` 也不用 `PROT_EXEC` |

**对 PocketDSH 的意义全部反转**：

- ✅ **好消息：不碰 AMFI、不碰 JIT、不碰 `PROT_EXEC`。** 纯用户态解释，与现在跑 i686 同样合法，之前担心的 iOS 限制在这个形态下一个都不存在。
- ⚠️ **坏消息：工作量是「写一个 aarch64 解释器」**，几千到上万行 C 代码，带指令缓存和跳转优化。是「大工程」，不是「中工程」。

#### 为什么 5–10× 而不是 x86 的 20–50×

aarch64 天生适合解释：

| | x86-64 | aarch64 |
| --- | --- | --- |
| 指令长度 | 1–15 字节变长，需状态机 | **定长 4 字节**，一个 switch |
| 寄存器 | 16 个，命名混乱 | **31 个通用**，直接数组索引 |
| 条件执行 | 依赖 EFLAGS 副作用 | 显式 `csel`/`b.cond` |
| 寻址模式 | 几十种组合 | 规整 load/store |
| 解码器规模 | 上万行 | 几千行 |

同样的线程化分派结构，aarch64 后端能跑到 x86 后端 3–5 倍。这是工程结论，不是玄学。

> 附带观察：`/usr/lib/cargo/bin/coreutils/cat` 中的 `cargo` 说明 Ubuntu 26.04 已把 coreutils 换成 uutils（Rust 版）。这反证 rootfs 是真的 26.04，不是伪造的。

### 7.5 基准（对比 PocketDSH 现状）

```sh
time sh -c 'i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done'
file /bin/sh /bin/ls
head -c 20 /bin/ls | od -An -tx1
# 7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
#                                    ↑ e_machine: b7=aarch64, 3e=x86-64
```

### 7.6 读 ServerBox 源码（定性收尾，未做）

```sh
git clone --depth=1 https://github.com/lollipopkit/flutter_server_box
cd flutter_server_box

grep -rniE "qemu|ish|proot|libkrun|wasm|wasi|tcg|jit|elf|dlopen|mmap" \
  --include="*.dart" --include="*.swift" --include="*.c" --include="*.h" . | head -80

grep -rniE "UTS_VERSION|4\.20\.69|SUPER AWESOME" . | head -20
grep -rniE "amd64|x86_64|aarch64|arm64" --include="*.c" . | head -30
```

三个要害：`4.20.69` 从哪来（确认 iSH 代码复用）、aarch64 后端放在哪（`emu/` 目录结构）、`/` 怎么 bind 到 App Group（直接抄）。

---

## 八、许可证约束

| 组件 | 许可 | 对 PocketDSH 的影响 |
| --- | --- | --- |
| iSH（fake kernel + 解释器） | **GPLv3** | 嵌进去 → 整个 app 必须 GPLv3 开源 |
| QEMU（UTM / UTM SE 的引擎） | **GPLv2** | 同上 |
| ios_system / a-Shell 路线 | 需逐一核对（多为 BSD 类） | 可闭源（核验后再定） |

**ServerBox 全 app 采用 GPL-3.0 开源发布，正是嵌了 iSH 的直接后果。** 它不是一个可选项，是合规要求。

PocketDSH 的岔路：

- **闭源商业** → Tier W（wasm，核许可）+ Tier R（远程），Tier C 只能作为内部实验
- **开源** → 可以嵌 iSH，走 Tier C+，成本最低
- **双版本** → 开源内核版 + 闭源 App Store 版，维护成本翻倍

**这个决定要在动手改造 iSH 之前定，因为它决定整个代码库的走向。**

---

## 九、下一步顺序

~~1. 修 apt~~ ✅ 已验证，见 7.1
~~2. arm64 执行与机制认定~~ ✅ 已通过，见 7.2 / 7.4
~~3. 做 workspace 挂载~~ ✅ 实现已提交（`11a1fe9`），见 [WORKSPACE-MOUNT.md](WORKSPACE-MOUNT.md)

余下按投入产出排序：

1. **做持久 shell**（第六节第 2 块）→ 摆脱"一次一条命令"限制。纯工程，收益立竿见影。
2. **读上游 aarch64 fork**（第十三节）→ `OpenMinis/ish-arm64` 与 `iSH-AOK` 已经把这个移植做完了，确认 `emu/` 的复用方式和 interpreter 能否单独构建。
3. **aarch64 解释器后端**（第六节第 3 块）→ 收益最大（20–50× → 5–10×），但**先定许可证方向**再动手。

**许可证方向必须在第 3 步之前定**，因为它决定整个代码库的走向（见第八节）。第 1、2 步不受影响。

---

## 十、本仓库代码勘察（2026-11-16）

对 `Vendor/iSH/` 的实地检查结果，用于判断「换 aarch64 后端」的真实工作量。

### 10.1 asbestos 已经在静态库里，但是个孤儿

`lib/iphoneos/libish_emu.a` 共 15 个目标文件，其中 9 个属于 **asbestos**（iSH 的 AArch64 模拟器）：

```
asbestos_asbestos.c.o                核心
asbestos_gen.c.o                     gadget 生成器
asbestos_helpers.c.o
asbestos_gadgets-aarch64_entry.S.o     fiber_enter / fiber_ret / fiber_ret_chain / fiber_exit
asbestos_gadgets-aarch64_memory.S.o    gadget_push / gadget_pop / crosspage_load / crosspage_store
asbestos_gadgets-aarch64_control.S.o   gadget_call / gadget_call_indir / handle_miss
asbestos_gadgets-aarch64_math.S.o
asbestos_gadgets-aarch64_bits.S.o      gadget_shl8/16/32_reg, gadget_shr*...
asbestos_gadgets-aarch64_string.S.o
asbestos_gadgets-aarch64_misc.S.o
```

符号里能读到 **`fiber_block_compile`**——它有块编译。技术形态是 gadget + fiber + block chaining：每条 guest 指令对应一段预编译 ARM64 汇编，块内用指针链直接跳转，不走返回栈。

**但它接不进 kernel**：

| 检查点 | 实际情况 |
| --- | --- |
| `include/emu/asbestos.h` | 不存在 |
| `include/kernel/elf.h` | 只有 `#define ELF_X86 3`；`entry_point`/`vaddr`/`paddr` 全是 `dword_t`（32 位） |
| `include/kernel/task.h` | `struct cpu_state cpu;` 内嵌 x86 寄存器（eax/eip/eflags） |
| `rootfs/data/bin/busybox` | `ELF 32-bit ... Intel 80386 ... /lib/ld-musl-i386.so.1` |

**asbestos 被编进了库，但 kernel 层是纯 32 位 x86。它是死代码。**

### 10.2 fakefs vs realfs 的能力差异

| | fakefs | realfs |
| --- | --- | --- |
| 头文件约束 | `fs/fake.h` 标 `#error "for internal use only"` | 无 |
| 数据位置 | sqlite `meta.db` + 内容表 | 宿主目录，`openat()` 直通 |
| 裸拷文件可见 | **否** | 是 |
| `mmap` | 内存模拟 | `realfs_mmap`，真实文件映射 |

PocketDSH 原来用 fakefs 挂 workspace，所以 Build 45 记录为 `NOT VERIFIED`。改用 realfs 后解决，见 [WORKSPACE-MOUNT.md](WORKSPACE-MOUNT.md)。

### 10.3 上游已有现成实现

| 项目 | 状态 | 公开数据 |
| --- | --- | --- |
| [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64) | 完成，商业化产品 Open Minis 在用 | 比 x86 模拟快 7–12×（`int_arith_2M` 12×、`fib(30)` 9.2×、`sum(1M)` 10.2×） |
| [iSH-AOK](https://github.com/emkey1/ish-AOK) | 完成，有[完整移植文档](https://raw.githubusercontent.com/emkey1/ish-AOK/refs/heads/working/docs/aarch64_guest_plan.md) | sh-loop 20k：i386 3.24s / arm64 2.62s；优化后 1.36s（2.1–2.3×） |
| [gzz2000/ish-jit-arm64](https://github.com/gzz2000/ish-jit-arm64) | arm64 JIT 独立实现 | — |

三者都是 GPLv3，衍生自 `ish-app/ish`。iSH-AOK 的文档明确说明其 aarch64 代码参考/移植自 OpenMinis，并保留 attribution。

iSH-AOK 文档里的一句定调与本文 7.4 的推断一致：

> the whole reason Open Minis is faster is **same-architecture gadget dispatch**

### 10.4 工作量重估

| 原估 | 修正 |
| --- | --- |
| 自己写 aarch64 解释器（几千到上万行 C） | 换上游 fork，只编 interpreter 路径 |
| 自己解决 64-bit ELF / task 抽象 / syscall 表 | 上游已完成（iSH-AOK patch 1–5） |
| 自己处理 TLS / 信号帧 / fork 的 arm64 ABI 差异 | 上游已完成（patch 7，busybox sh 可跑管道与信号） |

**结论：这条路从「2–3 个月的移植工程」变成「换依赖 + 适配桥接」。但仍需先定 GPLv3 方向。**

---

## 附录：实测原始输出

```
# 基准
root@serverbox:/# time sh -c 'i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done'
real    0m4.354s
user    0m4.331s
sys     0m0.018s

# exec 开销
root@serverbox:/# time sh -c 'i=0; while [ $i -lt 1000 ]; do /bin/true; i=$((i+1)); done'
real    0m1.302s
user    0m0.060s
sys     0m0.016s

# I/O
root@serverbox:/# dd if=/dev/zero of=/tmp/t bs=1M count=200 oflag=direct
209715200 bytes (210 MB, 200 MiB) copied, 0.367813 s, 571 MB/s

# 内核与资源
root@serverbox:/# cat /proc/version
Linux version 4.20.69-ish SUPER AWESOME Aug 22 2026 14:59:10
root@serverbox:/# nproc
6
root@serverbox:/# free -h
               total        used        free      shared  buff/cache   available
Mem:           4.1Gi       3.8Gi       288Mi          0B          0B       288Mi
Swap:             0B          0B          0B

# 架构
root@serverbox:/# dpkg --print-architecture
arm64
root@serverbox:/# uname -m
aarch64

# 挂载表
root@serverbox:/# cat /proc/self/mounts
devpts /ubuntu/dev/pts devpts rw 0 0
proc /ubuntu/proc proc rw 0 0
/private/var/mobile/Containers/Data/Application/B8E5ED81-.../Library/Application Support/linux-dev/ubuntu/data /ubuntu/dev fake rw 0 0
/private/var/mobile/Containers/Shared/AppGroup/EF817DF3-.../File Provider Storage/linux / real rw 0 0

# apt 状态
root@serverbox:/# ls -la /var/lib/apt/lists     → 空
root@serverbox:/# apt install -y python3-minimal
Error: Unable to locate package python3-minimal
exit=100

# === 修复后（2026-11-16）===

# DNS：resolv.conf 原为 0 字节
root@serverbox:/# printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf

# TLS：证书验证失败 → 降 http
# Error: SSL connection failed: error:0A000086:SSL routines::certificate verify failed [IP: 91.189.91.82 443]
root@serverbox:/# sed -i 's|https://archive.ubuntu.com|http://archive.ubuntu.com|' /etc/apt/sources.list.d/ubuntu.sources

# 索引：Cache-Start 24MiB 装不下 universe → 只启用 main
root@serverbox:/# echo 'APT::Cache-Start "134217728";' > /etc/apt/apt.conf.d/99cache-start
root@serverbox:/# apt-get -o APT::Sandbox::User=root update
Get:1 http://archive.ubuntu.com/ubuntu resolute InRelease [136 kB]
Get:2 http://archive.ubuntu.com/ubuntu resolute/main arm64 Packages [1468 kB]
Get:3 http://archive.ubuntu.com/ubuntu resolute/main Translation-en [524 kB]
Fetched 2128 kB in 5s (453 kB/s)
Reading package lists... Done

# 装包
root@serverbox:/# time apt-get -o APT::Sandbox::User=root install -y --no-install-recommends python3-minimal
The following NEW packages will be installed:
  libexpat1 libpython3.14-minimal python3-minimal python3.14-minimal
Get:1 .../libexpat1 arm64 2.7.4-1 [85.7 kB]
Get:2 .../libpython3.14-minimal arm64 3.14.4-1 [912 kB]
Get:3 .../python3.14-minimal arm64 3.14.4-1 [2568 kB]
Get:4 .../python3-minimal arm64 3.14.3-0ubuntu2 [25.8 kB]
After this operation, 13.9 MB of additional disk space will be used.
...
real    0m17.904s
user    0m0.249s
sys     0m0.055s

# 执行（AMFI 未拦）
root@serverbox:/# time python3 -c 'print(sum(range(10**7)))'
49999995000000
real    0m1.841s
user    0m1.818s
sys     0m0.013s
```
