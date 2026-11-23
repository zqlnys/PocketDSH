import bz2, gzip, hashlib, lzma, sqlite3, zlib

out = []
def t(name, fn):
    try:
        out.append(f"{name} {fn()}")
    except BaseException as e:
        out.append(f"{name} ERR {type(e).__name__} {e}")

data = b"pocketdsh" * 40
for algo in ("md5", "sha1", "sha256", "sha512", "blake2b", "sha3_256"):
    t(algo, lambda a=algo: hashlib.new(a, data).hexdigest()[:20])
t("zlib-rt", lambda: zlib.decompress(zlib.compress(data))[:9])
t("gzip-rt", lambda: gzip.decompress(gzip.compress(data))[:9])
t("bz2-rt", lambda: bz2.decompress(bz2.compress(data))[:9])
t("lzma-rt", lambda: lzma.decompress(lzma.compress(data))[:9])
t("zlib-crc", lambda: hex(zlib.crc32(data)))
t("adler", lambda: hex(zlib.adler32(data)))

def sqlite_case():
    db = sqlite3.connect(":memory:")
    db.execute("create table t(a integer, b text)")
    db.executemany("insert into t values (?, ?)", [(1, "x"), (2, "y")])
    rows = db.execute("select a, b from t order by a").fetchall()
    db.close()
    return rows
t("sqlite", sqlite_case)
print("\n".join(out))
