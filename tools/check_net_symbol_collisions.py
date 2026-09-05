#!/usr/bin/env python3
"""Проверка пересечения имён внутри одного translation unit.

main.c включает graphics.c, net.c и sound.c подряд, а те — свои части из
native/graphics и native/net. Всё это компилируется как один файл, поэтому
любое совпадение имён верхнего уровня (функция, переменная, typedef, макрос)
между «сетевой» и «графической» половинами — ошибка сборки. Локально net.c
обычно проверяют отдельно (gcc -fsyntax-only net.c), и такое совпадение там
не видно: падает только сборка в GitHub Actions.

Имена из net.h не считаются конфликтом: это общий интерфейс, объявление в
заголовке и определение в net.c совпадают по имени специально.

Запуск: python3 tools/check_net_symbol_collisions.py
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NET_PART = ['net.c'] + sorted(glob.glob(os.path.join(ROOT, 'native/net/*.inc')))
OTHER_PART = ['main.c', 'graphics.c', 'sound.c', 'net.h', 'runtime.h'] + \
    sorted(glob.glob(os.path.join(ROOT, 'native/graphics/*.inc')))
PUBLIC_HEADER = 'net.h'

DEFINE = re.compile(r'#\s*define\s+([A-Za-z_]\w*)')
STRUCT_END = re.compile(r'^\}\s*([A-Za-z_]\w*)\s*;')
TOKEN = re.compile(r'[A-Za-z_]\w*|\S')
IDENT = re.compile(r'[A-Za-z_]\w*$')
KEYWORDS = {'struct', 'union', 'enum', 'return', 'if', 'for', 'while', 'switch',
            'sizeof', 'else', 'do', 'case', 'typedef', 'const', 'static',
            'extern', 'inline', 'unsigned', 'signed', 'long', 'short', 'void'}


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def declared_name(line):
    """Имя, которое объявляет строка верхнего уровня (или None)."""
    if line.startswith('#'):
        m = DEFINE.match(line)
        return m.group(1) if m else None
    m = STRUCT_END.match(line)
    if m:
        return m.group(1)
    tokens = TOKEN.findall(line)
    if line.startswith('typedef'):
        # typedef struct { ... } Name; — имя последнее, перед точкой с запятой
        for tok in reversed(tokens):
            if IDENT.match(tok) and tok not in KEYWORDS:
                return tok
        return None
    # Обычное объявление: первый идентификатор, за которым идёт ( = ; [ {
    for i, tok in enumerate(tokens[:-1]):
        if IDENT.match(tok) and tok not in KEYWORDS and tokens[i + 1] in '(=;[{':
            return tok
    return None


def top_level_names(paths):
    """Имена, объявленные на верхнем уровне (строка начинается с первой колонки)."""
    found = {}
    for rel in paths:
        path = rel if os.path.isabs(rel) else os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        for line in strip_comments(open(path, encoding='utf-8').read()).split('\n'):
            if not line or line[0] in ' \t/)' or line.startswith('//'):
                continue
            name = declared_name(line)
            if name:
                found.setdefault(name, set()).add(os.path.relpath(path, ROOT))
    return found


def main():
    net = top_level_names(NET_PART)
    other = top_level_names(OTHER_PART)
    public = set(top_level_names([PUBLIC_HEADER]))
    clash = sorted((set(net) & set(other)) - public)
    if not clash:
        print('Symbol collision check: ok (%d net names, %d graphics names)'
              % (len(net), len(other)))
        return 0
    print('Имена верхнего уровня встречаются и в net.c, и в graphics-части '
          'main.c (один translation unit — будет ошибка сборки):')
    for name in clash:
        print('  %-24s %s | %s' % (name, ','.join(sorted(net[name])),
                                   ','.join(sorted(other[name]))))
    return 1


if __name__ == '__main__':
    sys.exit(main())
