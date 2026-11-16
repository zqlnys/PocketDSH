# Tier C 项目工作区挂载

状态：**模拟器已验证通过**（run 43793080884 / build 86 / commit `3092c26`）。真机未做。
最后更新：2026-11-16

目标：让 Tier C 里的 Linux 直接看到当前项目文件，`cd /workspace && ls` 与 Swift 侧文件树一致。

---

## 一、为什么之前的方案失败

Build 45 之前试过把项目树复制到 rootfs 下，再往 `meta.db` 的 `stats` / `paths` 表插记录。失败原因不是实现 bug，而是**选错了文件系统**。

iSH 有两套 fs 后端：

| 后端 | 类型名 | 数据在哪 | 适合什么 |
| --- | --- | --- | --- |
| `fakefs` | `fake` | sqlite 元数据库 `meta.db` + 内部内容表 | 只读 rootfs，纯内存/数据库模拟 |
| `realfs` | `real` | **宿主真实目录，经目录 fd + `openat()` 直通** | 需要与宿主共享文件的场景 |

`include/fs/fake.h` 开头就写明了：

```c
#ifndef ISH_INTERNAL
#error "for internal use only"
#endif
```

fakefs 是内部专用。文件内容的 inode 登记在 sqlite 里，**裸拷进目录的文件不在库中，guest 就看不见**——这正是 Build 45 记录的那条 `Tier C 项目工作区 | NOT VERIFIED`。

而且 fakefs 的"文件"是数据库行，和宿主文件没有共享 inode，写入还要反向同步，这本来就不是挂载项目目录的正确抽象。

---

## 二、方案：realfs 挂载

`realfs` 的挂载实现只有十几行（`fs/real.c`）：

```c
int realfs_mount(struct mount *mount) {
    char *source_realpath = realpath(mount->source, NULL);
    if (source_realpath == NULL) return errno_map();
    free((void *) mount->source);
    mount->source = source_realpath;
    mount->root_fd = open(mount->source, O_DIRECTORY);
    if (mount->root_fd < 0) return errno_map();
    return 0;
}
```

之后每个操作都是 `openat(mount->root_fd, fix_path(path), ...)`。**guest 和宿主共享同一批 inode**，没有复制、没有同步、没有元数据库。

所以挂载只要一行：

```c
do_mount(&realfs, host_workspace_path, "/workspace", "", 0);
```

关键是**不经过 `sys_mount`**，直接调 `do_mount()`，这样可以跳过 mount 命令的 type 字符串查找和目录校验，在初始化阶段就把挂载点摆好。

### 为什么不需要 iOSFS.m

iSH App 里的 `iOSFS.m` 解决的是 **security-scoped bookmark**——用户在 Files 里授权的外部目录，重启后要靠 bookmark 恢复访问权。

PocketDSH 的两种工作区：

| 工作区类型 | 路径在哪 | 需要 bookmark |
| --- | --- | --- |
| managed copy | App 容器 `Application Support/` | 否 |
| 外部授权文件夹 | 用户选的目录 | 是，但 `WorkspaceSession` 已经 `startAccessingSecurityScopedResource()` |

两种情况下，进程都持有访问权，realfs 的 `open()` 直接用绝对路径就能打开。**不需要引入 `iOSFS.m`。**

---

## 三、实现

### 挂载时机

必须在 `xX_main_Xx` 之后（rootfs 已挂到 `/`）、`task_run_current` 之前：

```c
int err = xX_main_Xx(6, argv, envp);
create_some_device_nodes();
do_mount(&procfs, "proc", "/proc", "", 0);
do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
pocket_mount_workspace();          // ← 新增
exit_hook = pocket_exit_hook;
task_run_current();
```

### 挂载诊断写进输出

挂载结果直接写到命令输出文件，UI 不用猜：

```
[workspace] mounted /path/to/project -> /workspace
```

失败时：

```
[workspace] host path missing: /path (errno=2)
[workspace] mount failed: errno=-2 (No such file or directory)
```

### 重复挂载

`mounts` 列表是全局的，跨 `pocket_ish_run` 调用会累积。挂载前先 `do_umount("/workspace")`，没有已存在挂载时它返回 `EINVAL`，无害。

### 传参链路

```
WorkspaceSession.rootURL
  → ToolExecutor.runLinux
    → LinuxRuntime.run(command:workspace:)
      → pocket_ish_run(rootfs, workspace, command, output)
        → pocket_mount_workspace()
          → do_mount(&realfs, workspace, "/workspace", "", 0)
```

空字符串表示不挂载（`thread_workspace[0] == '\0'` 提前返回）。

### 涉及文件

```
TierCProbe/ISHBridge.c                          挂载逻辑
TierCProbe/ISHBridge.h                          签名
TierCProbe/TierCProbeApp.swift                  probe 传空串
PocketDSH/Core/Services/LinuxRuntime.swift       加 workspace 参数
PocketDSH/Core/Services/ToolExecutor.swift       传 workspace.root
PocketDSH/Features/Settings/TierCSmokeView.swift 有会话时列出 /workspace
```

---

## 四、验证

### 验收标准

主 App Tier C 里执行：

```sh
uname -a
ls -la /workspace
cat /workspace/README.md
```

输出中的 `ls` 应当与 Swift 文件树一致，`cat` 应当读出真实内容。

### 已验证部分

| 项 | 状态 |
| --- | --- |
| 本地 Swift 语法解析（47 文件） | PASS |
| SwiftPM 测试 | 8/8 PASS |
| 本地 guardrail 扫描 | PASS |
| iSH 静态库符号（`realfs`、`do_mount`、`do_umount` 均导出） | 已确认 |
| CI 构建 | PASS（run 43791040100 / 43791854792 / 43793080884） |
| 模拟器内挂载 + 双向读写 | **PASS**（run 43793080884） |
| 真机 | 未做 |

#### 实测输出（run 43793080884）

```
[workspace] mounted /Users/runner/.../workspace -> /workspace
i686

[host readback] .tierc-probe.txt = tierc-write-ok
[host readback] workspace mount verified: guest write reached the host directory.
```

证据：[tierc/workspace-mounted.txt](tierc/workspace-mounted.txt)、
[tierc/workspace-mounted.png](tierc/workspace-mounted.png)

guest 写入的文件被宿主读回，证明 realfs 不是只读空壳，而是双向共享 inode 的活挂载。

#### 一个仍需注意的捕获限制

`ls -la /workspace` 的输出没有出现在截图里。i686 模拟下每条外部命令要 1–3 秒，而观察窗口约 5 秒，只有前一到两条命令能落进窗口。

**这是单命令桥接的捕获时序问题，不是挂载问题**——host readback 是时序无关的证明。持久 shell 会取代这个固定窗口。

**在 CI 与模拟器实测通过前，不要把 Tier C 标记为支持项目文件。**

### 风险点

1. **`realpath()` 在 iOS 沙盒路径上的行为**——`/var` 是指向 `/private/var` 的符号链接，`realpath` 会展开。realfs 保存的是展开后的路径，`open()` 能正常访问。
2. **security-scoped 外部目录**——如果 bookmark 失效或 `startAccessing` 失败，`open(O_DIRECTORY)` 返回 `EPERM`。诊断行会显示出来。
3. **`mounts` 列表累积**——已用 `do_umount` 兜底，但没做引用计数审计。
4. **写入回宿主的语义**——realfs 是直通，guest 写文件会直接改宿主文件。**这绕过了 `WorkspaceFileSystem` 的备份与路径守卫。** 只读挂载已经实现，见下一节；默认仍为读写。

---

## 五、只读挂载

`MS_READONLY_` 会被存进 `mount->flags`，但 **iSH 的读写路径从不检查它**——只传这个标志挂载，guest 照样能写。所以只读需要真正的拦截层。

实现方式：在 realfs 外面包一层 `fs_ops`（`readonly_realfs`）。

| 操作 | 行为 |
| --- | --- |
| `open`（`O_RDONLY`） | 转发 realfs |
| `open`（写模式 / `O_CREAT` / `O_TRUNC` / `O_APPEND`） | `EROFS` |
| `readlink`、`stat`、`fstat`、`getpath`、`statfs`、`flock` | 转发 realfs |
| `link`、`unlink`、`rmdir`、`rename`、`symlink`、`mknod`、`mkdir` | `EROFS` |
| `setattr`、`fsetattr`、`utime` | `EROFS` |
| `umount` | 关闭包装层接管的目录 fd |

挂载时会打印实际模式：

```
[workspace] mounted /path/to/project -> /workspace (read-only)
[workspace] mounted /path/to/project -> /workspace (read-write)
```

**默认仍是读写。** 是否默认只读是个产品决定：读写能让 guest 的构建/格式化工具正常工作，但会绕过工作区备份；只读更安全但限制用途。开关已经接好（`LinuxRuntime.workspaceReadOnly`，变化时自动重建会话），定下来只需改一行默认值。

状态：编译通过（build 96）。**运行时尚未验证**——用只读模式跑一次写命令、确认返回 `EROFS` 是下一步。

一个实现细节：`realfs_mount` 在 libish 里是导出符号，但 `fs/real.h` 没有声明它，包装层需要自己补原型，否则编译报 `use of undeclared identifier`。

## 六、后续

1. CI 通过后，在模拟器跑 `ls /workspace`，把输出存进 `docs/tierc/`。
2. 决定默认挂载模式（读写 / 只读）。能力已实现，默认值待产品确认。
3. 持久 shell 做好后，`/workspace` 跟随会话切换（卸载 + 重挂）。
