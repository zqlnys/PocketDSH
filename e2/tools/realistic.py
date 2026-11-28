# A short program of the kind someone would actually write: text in, several libraries, structured out.
# The per-API conformance script checks functions one at a time; this checks that they compose - that the
# output of one is accepted by the next with nothing lost in between.
import collections
import hashlib
import json
import re
import textwrap
import unicodedata

TEXT = """The quick brown fox jumps over the lazy dog.
Pack my box with five dozen liquor jugs.
How vexingly quick daft zebras jump!"""

words = re.findall(r"[a-z]+", TEXT.lower())
freq = collections.Counter(words).most_common(5)
print("words", len(words), "unique", len(set(words)))
print("top", freq)
lines = TEXT.splitlines()
print("lines", len(lines), "longest", max(len(line) for line in lines))
print("wrapped", textwrap.wrap(lines[0], 20))
print("sha", hashlib.sha256(TEXT.encode()).hexdigest()[:16])
blob = json.dumps({"freq": freq, "n": len(words)}, sort_keys=True)
print("json", blob)
print("json-roundtrip", json.loads(blob)["n"])
print("nfc", unicodedata.normalize("NFC", "e\u0301") == "\u00e9")
print("bigrams", collections.Counter(zip(words, words[1:])).most_common(3))
print("sorted-by-value", sorted(freq, key=lambda kv: (-kv[1], kv[0])))
