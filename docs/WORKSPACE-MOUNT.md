# Tier C 项目工作区挂载

状态：实现已提交（`11a1fe9`），等待 CI 与真机验证。
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
| CI 构建 | 待验证 |
| 模拟器内 `ls /workspace` | 待验证 |
| 真机 | 未做 |

**在 CI 与模拟器实测通过前，不要把 Tier C 标记为支持项目文件。**

### 风险点

1. **`realpath()` 在 iOS 沙盒路径上的行为**——`/var` 是指向 `/private/var` 的符号链接，`realpath` 会展开。realfs 保存的是展开后的路径，`open()` 能正常访问。
2. **security-scoped 外部目录**——如果 bookmark 失效或 `startAccessing` 失败，`open(O_DIRECTORY)` 返回 `EPERM`。诊断行会显示出来。
3. **`mounts` 列表累积**——已用 `do_umount` 兜底，但没做引用计数审计。
4. **写入回宿主的语义**——realfs 是直通，guest 写文件会直接改宿主文件。**这绕过了 `WorkspaceFileSystem` 的备份与路径守卫**，需要在 UI 上明确提示，或者后续加只读挂载（`do_mount` 的 `flags` 参数支持 `MS_READONLY_`）。

---

## 五、后续

1. CI 通过后，在模拟器跑 `ls /workspace`，把输出存进 `docs/tierc/`。
2. 考虑默认只读挂载，写入能力单独开关。
3. 持久 shell 做好后，`/workspace` 跟随会话切换（卸载 + 重挂）。
