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

def symlink_rt():
    with open("/tmp/target", "w") as f:
        f.write("data")
    os.symlink("/tmp/target", "/tmp/link")
    a = open("/tmp/link").read()
    b = os.path.islink("/tmp/link")
    c = os.readlink("/tmp/link")
    return "%s %s %s" % (a, b, c)

def hardlink_rt():
    with open("/tmp/h1", "w") as f:
        f.write("h")
    os.link("/tmp/h1", "/tmp/h2")
    s1, s2 = os.stat("/tmp/h1"), os.stat("/tmp/h2")
    return s1.st_ino == s2.st_ino and s1.st_nlink == 2

def mode_rt():
    with open("/tmp/m", "w") as f:
        f.write("m")
    os.chmod("/tmp/m", 0o640)
    return oct(stat.S_IMODE(os.stat("/tmp/m").st_mode))

def listdir_sorted():
    os.makedirs("/tmp/d", exist_ok=True)
    for n in "cab":
        open("/tmp/d/" + n, "w").close()
    return sorted(os.listdir("/tmp/d"))

def walk_depth():
    os.makedirs("/tmp/w/x/y", exist_ok=True)
    open("/tmp/w/x/y/z", "w").close()
    return sum(1 for _ in os.walk("/tmp/w"))

def stat_size():
    with open("/tmp/s", "w") as f:
        f.write("12345")
    return os.stat("/tmp/s").st_size

def isfile_isdir():
    os.makedirs("/tmp/fd", exist_ok=True)
    open("/tmp/ff", "w").close()
    return "%s %s" % (os.path.isfile("/tmp/ff"), os.path.isdir("/tmp/fd"))

def getcwd_chdir():
    old = os.getcwd()
    os.chdir("/tmp")
    now = os.getcwd()
    os.chdir(old)
    return now

def close_fd():
    f = open("/tmp/c", "w")
    os.close(f.fileno())
    return "ok"

t("symlink", symlink_rt)
t("hardlink", hardlink_rt)
t("mode", mode_rt)
t("listdir", listdir_sorted)
t("walk", walk_depth)
t("size", stat_size)
t("isfile", isfile_isdir)
t("cwd", getcwd_chdir)
t("close", close_fd)
t("stdin", lambda: sys.stdin is not None)

print("\n".join(out))
