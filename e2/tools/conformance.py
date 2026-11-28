# A wide sample of standard-library results, printed one per line so two backends can be compared value
# by value. This started as a survey that found three real defects - movi, the ASR in a shifted-register
# subtract, and log2's range reduction - none of which a test-per-feature approach had noticed.
import array, bisect, collections, decimal, fractions, functools, heapq, itertools
import math, operator, re, struct, unicodedata

cases = [
    ("int-big-mul", lambda: (2**64 * 3**40) % 1000000007),
    ("int-pow-mod", lambda: pow(3, 100, 7)),
    ("int-digits", lambda: len(str(2**4096))),
    ("int-bitcount", lambda: bin((1 << 200) - 1).count("1")),
    ("float-fmt", lambda: "%.17g" % (1 / 3)),
    ("float-repr", lambda: repr(0.1 + 0.2)),
    ("float-round-half", lambda: round(2.5)),
    ("float-fsum", lambda: math.fsum([0.1] * 10)),
    ("float-copysign", lambda: math.copysign(1, -0.0)),
    ("float-repeated-mul", lambda: functools.reduce(lambda a, _: a * 2.0, range(10), 1.0)),
    ("math-factorial", lambda: math.factorial(30) % 1000),
    ("math-comb", lambda: math.comb(30, 15)),
    ("math-isqrt", lambda: math.isqrt(10**20)),
    ("math-sqrt", lambda: math.sqrt(2.0)),
    ("math-exp", lambda: math.exp(1.0)),
    ("math-log1p", lambda: round(math.log1p(1e-10), 20)),
    ("math-expm1", lambda: round(math.expm1(1e-9), 20)),
    ("math-atan2", lambda: math.atan2(3.0, 4.0)),
    ("math-fmod", lambda: math.fmod(7.0, 3.0)),
    ("decimal-prec", lambda: str(decimal.Decimal("1") / decimal.Decimal("7"))[:12]),
    ("decimal-quant", lambda: str(decimal.Decimal("2.675").quantize(decimal.Decimal("0.01")))),
    ("fraction-limit", lambda: str(fractions.Fraction(1).limit_denominator(7))),
    ("struct-float", lambda: struct.unpack("<d", struct.pack("<d", 3.14159))[0]),
    ("struct-align", lambda: struct.calcsize("<qid")),
    ("struct-bigend", lambda: struct.pack(">I", 0xDEADBEEF).hex()),
    ("array-int64", lambda: list(array.array("q", [2**40, -1]))),
    ("bytes-translate", lambda: b"abc".translate(bytes.maketrans(b"a", b"z"))),
    ("bytes-find", lambda: b"abcabc".rfind(b"b")),
    ("str-format", lambda: "{:>8.2f}|{:x}|{:b}".format(3.14159, 255, 5)),
    ("str-encode-emoji", lambda: "\U0001F600".encode("utf-8").hex()),
    ("str-casefold", lambda: "Stra\u00dfe".casefold()),
    ("str-splitlines", lambda: "a\r\nb\nc".splitlines()),
    ("unicode-nfc", lambda: unicodedata.normalize("NFC", "e\u0301")),
    ("re-alternation", lambda: re.findall(r"ab|cd", "ab cd ab")),
    ("re-group", lambda: re.match(r"(\d+)-(\d+)", "12-34").groups()),
    ("re-lookbehind", lambda: re.search(r"(?<=a)b", "ab").group()),
    ("re-unicode", lambda: re.findall(r"\w+", "h\u00e9llo w\u00f6rld")),
    ("itertools-perm", lambda: len(list(itertools.permutations(range(5))))),
    ("itertools-product", lambda: len(list(itertools.product(range(3), repeat=3)))),
    ("functools-lru", lambda: (lambda f: (f(3), f(3))[0])(functools.lru_cache(None)(lambda n: n * 2))),
    ("deque-rotate", lambda: (lambda d: (d.rotate(1), list(d))[1])(collections.deque([1, 2, 3]))),
    ("counter-sub", lambda: (collections.Counter("aab") - collections.Counter("a")).most_common()),
    ("namedtuple", lambda: collections.namedtuple("P", "x y")(1, 2).x),
    ("heapq-nlargest", lambda: heapq.nlargest(2, [5, 1, 9, 3])),
    ("bisect-insort", lambda: (lambda a: (bisect.insort(a, 4), a)[1])([1, 2, 5])),
    ("operator-mul", lambda: operator.mul(6, 7)),
]

for name, fn in cases:
    try:
        print(name, repr(fn()))
    except Exception as exc:
        print(name, "ERR", type(exc).__name__)
