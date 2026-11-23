import base64, binascii, calendar, collections, copy, csv, datetime, decimal, difflib
import fractions, functools, heapq, hmac, html, http.client, io, itertools, json
import logging, operator, pathlib, pickle, random, re, shlex, statistics, string
import struct, textwrap, time, types, typing, unicodedata, urllib.parse, uuid, zlib

out = []
def t(name, fn):
    try:
        out.append(f"{name} {fn()}")
    except BaseException as e:
        out.append(f"{name} ERR {type(e).__name__}")

t("base64", lambda: base64.b64encode(b"pocket").decode())
t("b32", lambda: base64.b32encode(b"hi").decode())
t("zlib", lambda: zlib.compress(b"a"*50)[:4].hex())
t("crc", lambda: hex(binascii.crc32(b"abc") & 0xffffffff))
t("struct", lambda: struct.unpack("<ih", struct.pack("<ih", 7, -3)))
t("decimal", lambda: str(decimal.Decimal("1.1") + decimal.Decimal("2.2")))
t("fractions", lambda: str(fractions.Fraction(3, 6)))
t("statistics", lambda: statistics.mean([1, 2, 3, 4]))
t("diff", lambda: list(difflib.unified_diff("a\nb\n".splitlines(), "a\nc\n".splitlines(), n=0))[2:])
t("textwrap", lambda: textwrap.shorten("one two three four five", width=12))
t("shlex", lambda: shlex.split('a "b c" d'))
t("json", lambda: json.dumps(json.loads('{"z":1,"a":[true,null]}'), sort_keys=True))
t("pickle", lambda: pickle.loads(pickle.dumps({"a": (1, 2)})))
t("csv", lambda: list(csv.reader(io.StringIO("a,b\n1,2\n")))[1])
t("urllib", lambda: urllib.parse.urlparse("https://x/y?q=1").query)
t("uuid", lambda: str(uuid.UUID(int=0)))
t("hmac", lambda: hmac.new(b"k", b"m", "sha256").hexdigest()[:16])
t("html", lambda: html.unescape("&lt;b&gt;"))
t("heapq", lambda: heapq.nsmallest(2, [5, 1, 4]))
t("itertools", lambda: list(itertools.islice(itertools.count(5), 3)))
t("functools", lambda: functools.reduce(operator.mul, [1, 2, 3, 4]))
t("collections", lambda: collections.Counter("aab").most_common(1))
t("copy", lambda: copy.deepcopy({"a": [1]}))
t("datetime", lambda: datetime.date(2026, 1, 2).isoformat())
t("calendar", lambda: calendar.isleap(2024))
t("re-group", lambda: re.match(r"(?P<a>\d+)", "42").group("a"))
t("string", lambda: string.capwords("a b c"))
t("unicodedata", lambda: unicodedata.normalize("NFC", "e\u0301"))
t("typing", lambda: typing.get_origin(typing.List[int]).__name__)
print("\n".join(out))
