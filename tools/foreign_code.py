"""Keep code, names and hashes from closed or incompatible projects out of this repository.

The forbidden tokens are stored only as SHA-256 digests of their normalized spelling (lowercase letters and digits,
nothing else), so this file does not contain what it bans.

Two kinds of token:
  * long names are looked for as a substring of a line's normalized text, so every spelling is caught
    (camel case, underscores, dots, spaces);
  * short names are looked for as a whole word only (a line split on anything that is not a letter or digit, and at
    lower-to-upper case changes), so they do not fire inside unrelated identifiers.
A 64-digit hex token whose own SHA-256 is in HASH_FINGERPRINTS is a pinned hash of a module we do not use.

Standard library only. Used by tools/check_content.py; tests/foreign_code_check.py exercises it.
"""

import hashlib
import re

# Files that may name another project to credit it: the README and the release notes say what inspired this one.
# A name ending in "/" is a folder. Code never may.
CREDIT_FILES = {"README.md", "release-notes/"}

# length of normalized text -> digests
SUBSTRING_DIGESTS = {
    8: {
        "904556a61ed5f9ba420694e92efd10c728cbd8dbbc1b4671711e29db91dbfe9c",
        "ce4f9bd68a3d34b8295a3a6171110d854f8fc1f516777f986d28c5837796d8bc",
    },
    9: {
        "1be15ccec30825914362812ba935a204bce2b98c995ef4889aecfc184fade29c",
        "db0b45f73cff60aff1723cabb37601bc6ac212266831c7b5af02601e9a22bd32",
        "925904b477dec0a70df9f0d3c01bf26f21a4149cf261c691025598635dd3c4e2",
        "50da86f28908429b12b030393d7926621f5bf2ed8d123c12965630169e69f46a",
    },
    10: {
        "83440e961075788e5dbaac0c22c040de2704e4e0ab374675264dc8e9cdb6239b",
        "14145d806d5326a20e1b3f35e32e7254efea0d39224f212268e7870373b719e8",
    },
    11: {
        "2de4792e070b5946b414b7540fffa08628eb0cbea91c30d93dd06d9ccb5ec027",
        "0661efdb7b9fb15cfa248a83b320dbf939275d59c2e88664838f23156553f7ad",
    },
}

WORD_DIGESTS = {
    "10a68bdd3fe0233637d7e60aba0011ad065ccf41180c2bb887aa1192b6986ccf",
}

# SHA-256 of the lowercase hex text of two pinned module hashes.
HASH_FINGERPRINTS = {
    "be47b77cc8c7e049381722be2b46e826dd5d8c8622d43c7fd8fac74ac50c76dd",
    "a3366920f0cafe47ba8ce2572d05dd3133f1fe35ea4222d3eb211313efd33395",
}

_NON_ALNUM = re.compile(r"[^0-9a-z]+")
_WORD_SPLIT = re.compile(r"[^0-9A-Za-z]+|(?<=[a-z0-9])(?=[A-Z])")
_HEX64 = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{64}(?![0-9A-Fa-f])")


def _digest(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()


def forbidden_in_line(line: str, substrings=None, words=None, fingerprints=None) -> bool:
    """The three optional arguments replace the built-in digest tables (the test registers stand-ins)."""
    substrings = SUBSTRING_DIGESTS if substrings is None else substrings
    words = WORD_DIGESTS if words is None else words
    fingerprints = HASH_FINGERPRINTS if fingerprints is None else fingerprints
    norm = _NON_ALNUM.sub("", line.lower())
    for n, digests in substrings.items():
        for i in range(len(norm) - n + 1):
            if _digest(norm[i:i + n]) in digests:
                return True
    for w in _WORD_SPLIT.split(line):
        if w and _digest(w.lower()) in words:
            return True
    for m in _HEX64.finditer(line):
        if _digest(m.group(0).lower()) in fingerprints:
            return True
    return False


def forbidden_lines(text: str, **tables) -> list[int]:
    """1-based numbers of the lines that hold a forbidden token."""
    return [i for i, ln in enumerate(text.splitlines(), 1) if forbidden_in_line(ln, **tables)]
