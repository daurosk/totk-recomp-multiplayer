"""Checks tools/foreign_code.py: names and hashes from closed or incompatible projects stay out of the repository.

Stand-in tokens are registered for the samples, because the real ones are stored only as digests.

Run: python tests/foreign_code_check.py
"""

import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import check_content  # noqa: E402
import foreign_code as fc  # noqa: E402

failures = []


def expect(cond, what):
    if not cond:
        failures.append(what)


def d(s):
    return hashlib.sha256(s.encode()).hexdigest()


# Stand-ins: a long name (substring rule), a short name (whole-word rule), a pinned hash.
long_name, short_name, fake_hash = "zorblaxquux", "zq7", "ab" * 32
tables = dict(
    substrings={len(long_name): {d(long_name)}},
    words={d(short_name)},
    fingerprints={d(fake_hash)},
)


def hit(line):
    return fc.forbidden_in_line(line, **tables)


for line in [
    "// by ZorblaxQuux",
    "zorblax_quux = 1",
    "ZORBLAX.QUUX",
    "zorblax quux",
    "int zorblaxQuuxHandler;",
    "see zq7 here",
    "zq7_value",
    "Zq7Value",
    "H = '" + fake_hash.upper() + "'",
    "H = '" + fake_hash + "'",
]:
    expect(hit(line), f"not caught: {line!r}")

for line in [
    "int main(void) { return 0; }",
    "zorblax and quu",
    "azq7b",  # short name inside another word
    "zq",
    "H = '" + fake_hash[:-1] + "'",  # 63 digits
    "H = '" + fake_hash + "ab'",  # 66 digits
    "H = '" + "cd" * 32 + "'",
]:
    expect(not hit(line), f"flagged by mistake: {line!r}")

expect(fc.forbidden_lines("ok\nzorblaxquux\nok\n", **tables) == [2], "line numbers")

# Built-in tables: the right shape.
expect(sum(len(v) for v in fc.SUBSTRING_DIGESTS.values()) >= 5, "built-in substring table too small")
expect(len(fc.HASH_FINGERPRINTS) == 2, "built-in hash fingerprints")
for ds in fc.SUBSTRING_DIGESTS.values():
    expect(all(len(x) == 64 for x in ds), "bad digest length")

# Wiring: a clean probe file passes check_content.problems_for.
tmp = ROOT / "tests" / "_foreign_code_probe.txt"
try:
    tmp.write_text("nothing here\n", encoding="utf-8")
    expect(not [p for p in check_content.problems_for(tmp) if "closed or incompatible" in p], "clean file refused")
finally:
    tmp.unlink(missing_ok=True)

# Every tracked file is clean.
tracked = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
for rel in filter(None, tracked.decode("utf-8").split("\0")):
    for problem in check_content.problems_for(ROOT / rel):
        if "closed or incompatible" in problem:
            failures.append(f"{rel}: {problem}")

if failures:
    print("\n".join(failures))
    print(f"foreign_code_check: FAILED ({len(failures)})")
    sys.exit(1)
print("foreign_code_check: OK")
