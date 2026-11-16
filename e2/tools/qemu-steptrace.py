import sys
sys.path.insert(0, "/tmp")
from gdbmin import RSP
port = int(sys.argv[1]); start = int(sys.argv[2], 0); count = int(sys.argv[3])
r = RSP(port)
r._recv_packet() or r.cmd("?")
if r.cmd("Z1,%x,4" % start) is None:
    print("no reply to breakpoint"); raise SystemExit(1)
if r.cmd("z1,%x,4" % 0) is None:
    pass
for _ in range(60):
    resp = r.cmd("c")
    if resp is None or resp.startswith("W"):
        print("exited before 0x%x" % start); raise SystemExit(1)
    regs = r.regs()
    if regs and regs["pc"] == start:
        break
else:
    print("never reached 0x%x" % start); raise SystemExit(1)
out = [start]
for _ in range(count):
    resp = r.cmd("s")
    if resp is None or resp.startswith("W"):
        break
    regs = r.regs()
    if regs is None:
        break
    out.append(regs["pc"])
for pc in out:
    print("%x" % pc)
