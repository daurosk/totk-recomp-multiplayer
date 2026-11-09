"""totk.ini: set the game language and keep everything else (keys and syntax as runtime/cfg.c reads them).

A new file starts from totk.ini.example, whose lines are all commented defaults. The language goes into `lang`
(a Switch language code, runtime/ov_system.c): an active `lang = ...` line is rewritten in place, else the commented
`# lang = ...` line of the example is activated, else the line is added to [language] (created at the end if
missing). Other lines, comments and line order stay as they are.
"""
import os
import re
from pathlib import Path

# runtime/ov_system.c: the codes TOTK_LANG offers. Each language appears once, under the code the game accepts.
LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'nl', 'pt', 'pt-BR', 'ru', 'ko',
             'zh-Hans', 'zh-Hant')
# Older installer builds wrote these two codes; they are read as the canonical ones and rewritten on the next install.
ALIASES = {'zh-CN': 'zh-Hans', 'zh-TW': 'zh-Hant'}
# Their text falls back to the Switch shared fonts (runtime/ov_system.c), which the fonts step extracts from the
# firmware folder when the player gives one.
NEED_SHARED_FONTS = ('ko', 'zh-Hans', 'zh-Hant')

ACTIVE = re.compile(r'^(\s*)(?:totk_)?lang(\s*=\s*)(.*?)(\s*)$', re.IGNORECASE)
COMMENTED = re.compile(r'^\s*[#;]\s*(?:totk_)?lang\s*=.*$', re.IGNORECASE)
SECTION = re.compile(r'^\s*\[(.*)\]\s*$')


def canonical_language(lang):
    """The code LANGUAGES lists for lang: an alias of an older installer build is mapped, anything else is kept."""
    return ALIASES.get(lang, lang)


def set_language(text, lang):
    """text with `lang = <lang>` (an alias written as its canonical code); see the module docstring."""
    lang = canonical_language(lang)
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = ACTIVE.match(line)
        if m:
            lines[i] = f'{m.group(1)}lang{m.group(2)}{lang}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        if COMMENTED.match(line):
            lines[i] = f'lang = {lang}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        m = SECTION.match(line)
        if m and m.group(1).strip().lower() == 'language':
            lines.insert(i + 1, f'lang = {lang}')
            return '\n'.join(lines) + '\n'
    if lines and lines[-1].strip():
        lines.append('')
    lines += ['[language]', f'lang = {lang}']
    return '\n'.join(lines) + '\n'


def get_language(text):
    for line in text.splitlines():
        m = ACTIVE.match(line)
        if m:
            return m.group(3)
    return None


ADDRESS = re.compile(r'^(?:[A-Za-z0-9._-]+|\[[0-9A-Fa-f:.]+\]):([0-9]{1,5})$')


def valid_address(text):
    """True for host:port (a name, an IPv4 address or a bracketed IPv6 address) with a port of 1 to 65535."""
    m = ADDRESS.match(text.strip())
    return bool(m) and 0 < int(m.group(1)) < 65536


def set_key(text, section, key, value):
    """text with `key = value` under [section]: an active line is rewritten in place, else the commented `# key = ...`
    line of the example inside the section is activated, else the line is added at the section's top (the section is
    created at the end if missing). Everything else stays."""
    lines = text.splitlines()
    active = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
    commented = re.compile(r'^\s*[#;]\s*' + re.escape(key) + r'\s*=')
    start = end = None
    for i, line in enumerate(lines):
        m = SECTION.match(line)
        if m:
            if start is not None:
                end = i
                break
            if m.group(1).strip().lower() == section:
                start = i
    if start is None:
        if lines and lines[-1].strip():
            lines.append('')
        lines += [f'[{section}]', f'{key} = {value}']
        return '\n'.join(lines) + '\n'
    end = len(lines) if end is None else end
    for pattern in (active, commented):
        for i in range(start + 1, end):
            if pattern.match(lines[i]):
                lines[i] = f'{key} = {value}'
                return '\n'.join(lines) + '\n'
    lines.insert(start + 1, f'{key} = {value}')
    return '\n'.join(lines) + '\n'


def set_server(text, address):
    """text with the server to join: [online] net_address, and net_autoconnect = 1 so the game connects at start."""
    return set_key(set_key(text, 'online', 'net_address', address.strip()), 'online', 'net_autoconnect', '1')


def write(dest, lang, example=None, server=None):
    """Write dest/totk.ini with lang (and server, host:port, when given); an existing file keeps its other lines.
    Returns the path."""
    path = Path(dest) / 'totk.ini'
    if path.is_file():
        text = path.read_text(encoding='utf-8-sig')
    elif example and Path(example).is_file():
        text = Path(example).read_text(encoding='utf-8')
    else:
        text = ''
    new = set_language(text, lang)
    if server:
        new = set_server(new, server)
    if path.is_file() and new == text:
        return path
    tmp = path.with_name('totk.ini.tmp')
    tmp.write_text(new, encoding='utf-8', newline='\n')
    os.replace(tmp, path)
    return path
