"""Lexical helpers: literals, brackets, strings, comments and top-level splits.

These functions work on raw source text and know nothing about the compiler
state; tools/ds_lint.py reuses them."""

import re


_NAME = r'[A-Za-z_]\w*'
_NUM_RE = re.compile(r'^(?:[-+]?\d+(?:\.\d+)?|0[xX][0-9a-fA-F]+)$')
_LHS_RE = re.compile(r'^(' + _NAME + r')(?:\.(' + _NAME + r'))?$')
_FOR_RE = re.compile(
    r'^for\s+(' + _NAME + r')\s*=\s*(.+?)\s+(?:to)\s+(.+?)'
    r'(?:\s+(?:step)\s+(.+?))?\s+do$', re.IGNORECASE)
_COMPOUND_OPS = ('+=', '-=', '*=', '/=')
def num_value(text):
    """Numeric value of a literal, or None when it is not a plain number."""
    text = text.strip()
    if not _NUM_RE.match(text):
        return None
    try:
        return float(int(text, 16)) if text[:2].lower() == '0x' else float(text)
    except ValueError:
        return None


def scan(text):
    """Bracket depth and string literal mask for every character."""
    n = len(text)
    depth = [0] * n
    quoted = [False] * n
    in_str = False
    esc = False
    lvl = 0
    i = 0
    while i < n:
        c = text[i]
        quoted[i] = in_str
        if esc:
            esc = False
        elif in_str:
            if c == '\\':
                esc = True
            elif c == '"':
                in_str = False
        elif text.startswith('$"', i):
            in_str = True
            quoted[i] = False
            depth[i] = lvl
            i += 1
            quoted[i] = in_str
        elif c == '"':
            in_str = True
        elif c == '(':
            lvl += 1
        elif c == ')':
            lvl = max(0, lvl - 1)
        depth[i] = lvl
        i += 1
    return depth, quoted


def open_parens(text):
    """How many '(' are left unclosed, outside string literals."""
    _, quoted = scan(text)
    balance = 0
    for i, c in enumerate(text):
        if quoted[i]:
            continue
        if c == '(':
            balance += 1
        elif c == ')':
            balance -= 1
    return balance


def split_top(text, sep):
    depth, quoted = scan(text)
    parts = []
    start = 0
    for i, c in enumerate(text):
        if depth[i] == 0 and not quoted[i] and c == sep:
            parts.append(text[start:i].strip())
            start = i + 1
    parts.append(text[start:].strip())
    return parts


def strip_comment(line):
    """A v2 comment starts with '--' as in Lua; dashes in strings stay."""
    out = []
    i = 0
    in_str = False
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif line.startswith('$"', i):
            out.append('$"')
            in_str = True
            i += 2
            continue
        elif c == '"':
            in_str = True
            out.append(c)
        elif c == '-' and i + 1 < len(line) and line[i + 1] == '-':
            break
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def find_compound(line):
    """Position of a compound operator (+=, -=, *=, /=) outside strings and
    brackets."""
    depth, quoted = scan(line)
    for i, c in enumerate(line):
        if quoted[i] or depth[i] or i + 1 >= len(line):
            continue
        if line[i + 1] == '=' and c in '+-*/':
            return i
    return -1


def find_assign(line):
    depth, quoted = scan(line)
    for i, c in enumerate(line):
        if quoted[i] or depth[i]:
            continue
        if c == '=' and (i == 0 or line[i - 1] not in '<>!') and (i + 1 >= len(line) or line[i + 1] != '='):
            return i
    return -1


def used_outside_strings(text, name):
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    _, quoted = scan(text)
    return any(not quoted[m.start()] for m in pat.finditer(text))


def sub_unquoted(e, pat, repl):
    """Replace pat with repl outside string literals."""
    _, quoted = scan(e)
    if not any(quoted):
        return pat.sub(repl, e)
    out = []
    start = 0
    for m in pat.finditer(e):
        if quoted[m.start()]:
            continue
        out.append(e[start:m.start()])
        out.append(m.expand(repl))
        start = m.end()
    out.append(e[start:])
    return ''.join(out)


def interp_holes(text):
    """Parses $"...{expr}...": a list of ('lit', s) and ('hole', expr)."""
    parts = []
    i = 2
    lit = []
    while i < len(text):
        c = text[i]
        if c == '\\' and i + 1 < len(text):
            lit.append(text[i + 1])
            i += 2
            continue
        if c == '"':
            break
        if c == '{':
            depth = 1
            j = i + 1
            while j < len(text) and depth:
                if text[j] == '{':
                    depth += 1
                elif text[j] == '}':
                    depth -= 1
                j += 1
            if lit:
                parts.append(('lit', ''.join(lit)))
                lit = []
            parts.append(('hole', text[i + 1:j - 1]))
            i = j
            continue
        lit.append(c)
        i += 1
    if lit:
        parts.append(('lit', ''.join(lit)))
    return parts
