"""Exercises the syscalls a Python program actually reaches, one per line.

Every line is either a value or an error code. The expected output below is what qemu produces for
the same file, so a line that differs is a difference between this backend and an independent one -
which is the only kind of disagreement worth acting on.
"""
import errno, os, select, stat, time

out = []


def t(name, fn):
    try:
        out.append("%s %s" % (name, fn()))
    except OSError as e:
        out.append("%s E%d" % (name, e.errno))
    except Exception as e:
        out.append("%s X %s" % (name, type(e).__name__))


def errno_of(fn):
    try:
        fn()
        return "no-error"
    except OSError as e:
        return "E%d" % e.errno


t("getpid", lambda: os.getpid() > 0)
t("getppid", lambda: os.getppid() >= 0)
t("umask", lambda: isinstance(os.umask(0o022), int))
t("stat-enoent", lambda: errno_of(lambda: os.stat("/nope")))
t("unlink-enoent", lambda: errno_of(lambda: os.unlink("/tmp/nope")))
t("open-enoent", lambda: errno_of(lambda: open("/tmp/nope", "r")))


def mkdir_rmdir():
    os.mkdir("/tmp/pd1")
    os.rmdir("/tmp/pd1")
    return "ok"


def rename_rt():
    with open("/tmp/a", "w") as f:
        f.write("x")
    os.rename("/tmp/a", "/tmp/b")
    return "ok"


def pipe_rt():
    r, w = os.pipe()
    os.write(w, b"ping")
    data = os.read(r, 4)
    os.close(r)
    os.close(w)
    return data.decode()


def select_ready():
    r, w = os.pipe()
    os.write(w, b"z")
    ready = select.select([r], [], [], 1)[0]
    os.close(r)
    os.close(w)
    return len(ready) == 1


def select_empty():
    return select.select([], [], [], 0.01) == ([], [], [])


def fd_blocking():
    with open("/tmp/fl", "w") as f:
        return isinstance(os.get_blocking(f.fileno()), bool)


t("mkdir-rmdir", mkdir_rmdir)
t("rename", rename_rt)
t("pipe", pipe_rt)
t("select", select_ready)
t("select-empty", select_empty)
t("fd-blocking", fd_blocking)
t("environ", lambda: "PATH" in os.environ or "HOME" in os.environ)
t("time", lambda: time.time() > 1600000000)
t("monotonic", lambda: time.monotonic() >= 0)
t("lseek", lambda: (open("/tmp/seek", "w").write("abcdef"), "ok")[1])


def seek_rt():
    with open("/tmp/seek2", "w+") as f:
        f.write("abcdef")
        f.seek(2)
        return f.read(2)


t("seek-read", seek_rt)

print("\n".join(out))
