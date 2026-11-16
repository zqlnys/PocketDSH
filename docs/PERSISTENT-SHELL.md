# Tier C 持久 shell

目标：让 Tier C 能连续执行多条命令，端口输出不因固定观察窗口而截断。

状态：**模拟器已验证通过**（run 43796926038 / build 89 / commit `72fbe9f`）。真机未做。

---

## 一、为什么需要它

原来的 `pocket_ish_run` 每执行一条命令就做一遍完整的内核启动：

```
xX_main_Xx → create_some_device_nodes → mount proc/devpts → task_run_current
```

然后宿主 `dup2` 全局 stdout 到文件，`sleep 5` 秒，再读文件。两个结构性缺陷：

1. **每条命令一次内核启动。** i686 模拟下，一条外部命令要 1–3 秒，5 秒窗口只装得下一两条。
2. **输出边界靠猜。** `sleep` 到期就读，最后一条命令的输出经常还在路上——Build 46 到 Build 85 的截图里反复出现「`ls /workspace` 没有输出」，就是这个原因。

实测代价：Build 85 的 smoke 显示 `uname -a` 输出正常，而它后面的 `cat`、`ls` 全部丢失。这不是命令失败，是捕获时机。

---

## 二、设计

```
宿主                           guest（iSH fake kernel）
──────                         ────────────────────────
to_guest[1] ──write──> pipe ──> fd 0  (/bin/sh 的 stdin)
from_guest[0] <──read── pipe <── fd 1, 2 (stdout + stderr)
```

要点：

- **pipe 端通过 `adhoc_fd_create(&realfs_fdops)` 注入 guest 的 fd 表**，与 iSH 自己的 `create_piped_stdio()` 同一手法。`realfs_fdops` 的 read/write 直接落到宿主 fd，中间没有缓冲层。
- **`/bin/sh` 不带 `-c`**，从 stdin 逐行读命令，天然循环。
- **命令边界用 sentinel**：每条命令发出去时追加 `echo __POCKET_DONE__`，宿主轮询读端直到看见它。这样边界来自 shell 自己，不依赖任何时间假设。
- **会话串行化**：iSH 只有一份全局 mount 列表、pid 表和 fakefs 数据库，两个并发 shell 会互相踩。用一把 `pthread_mutex_trylock` 保证单会话。

### 关键时序

```
pocket_shell_start()
  ├─ pipe() ×2
  ├─ pthread_create(shell_thread)
  └─ 返回（不等 shell 就绪）

shell_thread:
  ├─ xX_main_Xx(4, {"ish","-f",rootfs,"/bin/sh"})
  ├─ create_some_device_nodes()
  ├─ do_mount(procfs / proc)  do_mount(devptsfs / dev/pts)
  ├─ pocket_mount_workspace()          ← realfs 直通
  ├─ install_stdio()                   ← 把 pipe 塞进 files[0..2]
  └─ task_run_current()                ← 阻塞到 shell 退出
```

`install_stdio()` 在 `task_run_current()` 之前覆盖 `current->files->files[0..2]`。此时 `/bin/sh` 已经 exec 过，但 fd 表是 task 的属性，覆盖指针后 shell 下一次 `read(0)` 就走新 pipe。

---

## 三、踩到的坑：exit_hook 杀掉了会话

**这是让「持久 shell 原型未稳定」的那个 bug。**

iSH 的 `exit_hook` 是全局函数指针，**任何 task 退出都会调用它**，包括 shell 命令 fork 出来的子进程。

一次性 runner 里因为只有一个 task，`pthread_exit(NULL)` 是对的。但在持久 shell 里：

```
[3/6 +0.1s] $ echo tierc-write-ok > /workspace/.tierc-probe.txt
[4/6 +20.2s] $ cat /workspace/.tierc-probe.txt
tierc-write-ok              ← 子进程跑完了，输出写进 pipe
<output truncated>          ← 但 sentinel 永远不来
[5/6 +20.2s] $ uname -m
<shell error -32>           ← 会话已经没了
```

`cat` 退出 → `exit_hook` → `pthread_exit` → 驱动 shell 的宿主线程被杀死。子进程的输出在死之前已经写进 pipe，所以看起来「命令跑了但会话没了」。

修法：在 `task_run_current()` 之前记下 shell 自己的 pid，hook 里只对这一个 pid 生效。

```c
if (task != NULL && g_shell_pid != 0 && task->pid != g_shell_pid) {
    return;   // 子进程退出，shell 还活着
}
```

---

## 四、API

```c
pocket_shell *pocket_shell_start(const char *rootfs, const char *workspace,
                                 char *error, size_t error_size);

long pocket_shell_exec(pocket_shell *shell, const char *command,
                       char *out, size_t out_size, int timeout_ms,
                       int *truncated);

int  pocket_shell_is_running(pocket_shell *shell);
void pocket_shell_stop(pocket_shell *shell);
```

Swift 侧 `PersistentShellSession` 包一层，`run(_:timeout:)` 返回合并输出。

---

## 五、限制与已知风险

1. **单会话**。第二個 `start()` 会返回 `another shell session is active`。
2. **不能与一次性 `pocket_ish_run` 并发**。两者共用 iSH 全局状态，目前没有互斥保护——smoke 页面只走持久 shell，但接进 `run_linux` 前必须处理。
3. **sentinel 可能被命令内容干扰**。如果命令本身就打印 `__POCKET_DONE__`，边界会提前闭合。实际命令里不会出现。
4. **优雅退出失败时泄漏**。`exit` 命令 3 秒内没让 shell 停下，就 `pthread_detach` 并故意不释放结构体——`task_run_current` 里没有 cancellation point，join 会永久阻塞，而 free 会让线程碰已释放内存。
5. **每条命令 12–20 秒超时**。外部命令在 i686 下不快；超时后返回已收到的部分并标记 `truncated`。
6. **真机未验证**。

---

## 六、验证记录

脚本（Tier C smoke 页面）：

```
pwd
uname -m                                              # 外部命令，会 fork
echo alive-after-external                             # 证明会话活过了上一条
echo tierc-write-ok > /workspace/.tierc-probe.txt
read v < /workspace/.tierc-probe.txt; echo got:$v     # builtin 读挂载
ls /workspace                                         # 再来一条外部命令
```

`uname -m` 被刻意放在第二条：如果子进程退出的修复回退了，它会立刻暴露，而不是藏在几条 builtin 后面。

验收标准：

- 六条命令全部返回非空输出；
- `alive-after-external` 出现（会话没被子进程带走）；
- `got:tierc-write-ok`（builtin 能读 realfs 挂载）；
- `still running: yes`；
- 宿主侧 readback 仍然读到 `tierc-write-ok`。

### 实测结果（run 43796926038 / build 89）

```
Persistent shell started.

[1/6 +0.1s] $ pwd
/
[2/6 +0.1s] $ uname -m
i686
[3/6 +0.1s] $ echo alive-after-external
alive-after-external
[4/6 +0.1s] $ echo tierc-write-ok > /workspace/.tierc-probe.txt
[5/6 +0.1s] $ read v < /workspace/.tierc-probe.txt; echo got:$v
got:tierc-write-ok
[6/6 +0.1s] $ ls /workspace
README.md
still running: yes

[host readback] .tierc-probe.txt = tierc-write-ok
[host readback] workspace mount verified: guest write reached the host directory.
```

验收标准全部达成：

| 项 | 结果 |
| --- | --- |
| 六条命令在**同一个会话**里连续执行 | ✅ |
| 外部命令（`uname -m`）退出后会话存活 | ✅ `alive-after-external` |
| builtin 能读 realfs 挂载 | ✅ `got:tierc-write-ok` |
| `ls /workspace` 看到真实项目文件 | ✅ `README.md` |
| 会话可复用 | ✅ `still running: yes` |
| 宿主 readback 仍然匹配 | ✅ |

**意外收获：每条命令 0.1 秒。** 之前 Build 46–85 里每条 1–3 秒的开销，全部来自「每条命令重起一次内核」（`xX_main_Xx` + mounts），不在命令本身。保留一个 shell 就把这笔开销抹掉了。

证据：[tierc/persistent-shell.txt](tierc/persistent-shell.txt)、[tierc/persistent-shell.png](tierc/persistent-shell.png)
