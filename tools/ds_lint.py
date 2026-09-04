#!/usr/bin/env python3
"""Статическая проверка скриптов DimScript (game/scripts/*.ds).

Компилятор DimScript (ds_compiler.py) устроен мягко: присваивание неизвестной
переменной он МОЛЧА выбрасывает из game.c, а чтение неизвестного имени уезжает
в C и валит уже сборку Android. Обе ошибки ловятся только на поздних шагах,
поэтому здесь они ищутся заранее, до компиляции C:

  * присваивание `имя = ...`, где имя не объявлено (глобально, параметром или
    локально выше по функции);
  * чтение идентификатора, которого нигде нет;
  * вызов функции скрипта с другим числом аргументов;
  * `name.field`, где у объекта нет такого поля.

Проверка намеренно не повторяет компилятор: она не генерирует код, а только
сверяет имена, и потому падает на опечатках, которые компилятор проглатывает.
Запуск: python3 tools/ds_lint.py [каталог-со-скриптами]
"""

from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ds_compiler import BUILTINS, ENGINE_VARS, STR_BUILTINS, strip_comment, split_top  # noqa: E402
from gen import find_ds_files  # noqa: E402

_NAME = r'[A-Za-z_]\w*'
_DECL_RE = re.compile(r'^(number|string|color|array|' + _NAME + r')\s+(.+)$')
_TYPES = {'number', 'string', 'color', 'array', 'num', 'str', 'col', 'arr'}
# В бою есть несколько мест, где в выражение вставлен честный C-каст:
# такие слова не имена скрипта, а часть нативного выражения.
_RAW_C_WORDS = {'unsigned', 'int', 'double', 'char', 'float'}
# Математика из math.h, которую подключает сгенерированный game.c: fabs там
# настоящая сишная, хотя в BUILTINS компилятора её нет.
_NATIVE_MATH = {'fabs'}
_NUM_RE = re.compile(r'\b0[xX][0-9a-fA-F]+\b|\b\d+(?:\.\d+)?\b')


def iter_blocks(lines):
    """Разбивает строки модуля на ('decl'|'function', name, params, body)."""
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r'^function\s+(' + _NAME + r')\s*(.*)$', line)
        if m:
            body, i = collect_block(lines, i + 1)
            yield 'function', m.group(1), parse_params(m.group(2)), body
            continue
        i += 1


def collect_block(lines, i):
    depth = 0
    body = []
    while i < len(lines):
        line = lines[i]
        if line == 'end':
            if depth == 0:
                return body, i + 1
            depth -= 1
        elif line.startswith('if ') or line.startswith('loop '):
            depth += 1
        body.append(line)
        i += 1
    return body, i


def parse_params(text):
    text = (text or '').strip()
    if text.startswith('(') and text.endswith(')'):
        text = text[1:-1].strip()
    params = []
    for part in split_top(text, ',') if text else []:
        words = part.split()
        if len(words) == 2:
            params.append(words[1])
    return params


def identifiers(text):
    """Идентификаторы выражения вне строковых литералов и чисел."""
    depth, quoted = _scan(text)
    text = _NUM_RE.sub(lambda m: ' ' * len(m.group(0)), text)
    out = []
    for m in re.finditer(_NAME, text):
        if quoted[m.start()] or depth[m.start()]:
            continue
        out.append((m.group(0), m.start()))
    return out


def _scan(text):
    n = len(text)
    depth = [0] * n
    quoted = [False] * n
    lvl, in_str, esc = 0, False, False
    for i, c in enumerate(text):
        quoted[i] = in_str
        if esc:
            esc = False
            continue
        if in_str:
            if c == '\\':
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == '(':
            lvl += 1
        elif c == ')':
            lvl = max(0, lvl - 1)
        depth[i] = lvl
    return depth, quoted


class Lint:
    def __init__(self):
        self.globals = {}          # имя -> тип ('object:Name' для объектов)
        self.objects = {}          # имя -> set(поля)
        self.functions = {}        # имя -> [параметры]
        self.errors = []

    def error(self, where, msg):
        self.errors.append(f'{where}: {msg}')

    def scan_declarations(self, lines, where):
        for line in lines:
            m = _DECL_RE.match(line)
            if not m:
                continue
            if m.group(1) not in _TYPES and m.group(1) not in self.objects:
                continue
            for part in split_top(m.group(2), ','):
                mm = re.match(r'^(' + _NAME + r')\s*(?:=.*)?$', part.strip())
                if mm:
                    self.globals[mm.group(1)] = m.group(1)

    def scan_objects(self, lines):
        i = 0
        while i < len(lines):
            m = re.match(r'^object\s+(' + _NAME + r')', lines[i])
            if not m:
                i += 1
                continue
            name = m.group(1)
            fields = set()
            i += 1
            while i < len(lines) and lines[i] != 'end':
                fm = _DECL_RE.match(lines[i])
                if fm and fm.group(1) in _TYPES:
                    for part in split_top(fm.group(2), ','):
                        pm = re.match(r'^(' + _NAME + r')', part.strip())
                        if pm:
                            fields.add(pm.group(1))
                i += 1
            self.objects[name] = fields
            i += 1

    def known(self, name):
        return (name in self.globals or name in self.functions
                or name in BUILTINS or name in ENGINE_VARS
                or name in STR_BUILTINS or name in _RAW_C_WORDS
                or name in _NATIVE_MATH)

    def check_function(self, where, name, params, body):
        scope = set(params)
        for line in body:
            m = _DECL_RE.match(line)
            if m and m.group(1) in _TYPES:
                for part in split_top(m.group(2), ','):
                    pm = re.match(r'^(' + _NAME + r')\s*(?:=(.*))?$', part.strip())
                    if pm:
                        scope.add(pm.group(1))
                        if pm.group(2):
                            self.check_expression(where, name, pm.group(2), scope)
                continue
            if line.startswith('else if '):
                self.check_expression(where, name, line[8:], scope)
                continue
            if line.startswith('if ') or line.startswith('loop '):
                self.check_expression(where, name, line.split(' ', 1)[1], scope)
                continue
            if line.startswith('return '):
                self.check_expression(where, name, line[7:], scope)
                continue
            if line in ('return', 'end', 'else'):
                continue
            i = find_assign(line)
            if i >= 0:
                lhs, rhs = line[:i].strip(), line[i + 1:].strip()
                self.check_lhs(where, name, lhs, scope)
                self.check_expression(where, name, rhs, scope)
                continue
            cm = re.match(r'^(' + _NAME + r')\s*(?:\((.*)\))?$', line)
            if cm:
                self.check_call(where, name, cm.group(1), cm.group(2) or '', scope)

    def check_lhs(self, where, fn, lhs, scope):
        m = re.match(r'^(' + _NAME + r')(?:\.(' + _NAME + r'))?$', lhs)
        if not m:
            return
        name, field = m.group(1), m.group(2)
        if name not in scope and name not in self.globals and name not in ENGINE_VARS:
            self.error(where, f"функция '{fn}': присваивание необъявленной "
                              f"переменной '{name}' (компилятор молча её выбросит)")
            return
        if field:
            holder = self.globals.get(name)
            fields = self.objects.get(holder)
            if fields is not None and field not in fields:
                self.error(where, f"функция '{fn}': у объекта '{holder}' нет поля '{field}'")

    def check_expression(self, where, fn, expr, scope):
        for ident, _pos in identifiers(expr):
            if ident in scope or self.known(ident):
                continue
            self.error(where, f"функция '{fn}': неизвестное имя '{ident}' в выражении")
        depth, quoted = _scan(expr)
        for m in re.finditer(r'\b(' + _NAME + r')\s*\(', expr):
            if quoted[m.start()]:
                continue
            self.check_call(where, fn, m.group(1), args_of(expr, m.end()), scope)
        for m in re.finditer(r'\b(' + _NAME + r')\.(' + _NAME + r')\b', expr):
            if quoted[m.start()]:
                continue
            holder = m.group(1)
            if holder in scope or holder in self.globals:
                fields = self.objects.get(self.globals.get(holder) or '', set())
                if fields and m.group(2) not in fields:
                    self.error(where, f"функция '{fn}': у объекта '{holder}' нет поля '{m.group(2)}'")

    def check_call(self, where, fn, name, args_text, scope):
        if name in self.functions:
            args = split_top(args_text, ',') if args_text.strip() else []
            want = len(self.functions[name])
            if len(args) != want:
                self.error(where, f"функция '{fn}': вызов '{name}' ждёт {want} "
                                  f"аргумент(а), передано {len(args)}")
        elif name not in BUILTINS and name not in _RAW_C_WORDS and name not in _NATIVE_MATH:
            self.error(where, f"функция '{fn}': неизвестный вызов '{name}'")


def find_assign(line):
    depth, quoted = _scan(line)
    for i, c in enumerate(line):
        if quoted[i] or depth[i]:
            continue
        if c == '=' and (i == 0 or line[i - 1] not in '<>!') and line[i + 1:i + 2] != '=':
            return i
    return -1


def args_of(expr, open_paren):
    depth = 0
    for i in range(open_paren - 1, len(expr)):
        if expr[i] == '(':
            depth += 1
        elif expr[i] == ')':
            depth -= 1
            if depth == 0:
                return expr[open_paren:i]
    return expr[open_paren:]


def lint_dir(scripts):
    """Проверяет каталог со скриптами, возвращает список строк-ошибок."""
    sources = find_ds_files(scripts)
    modules = {}
    for path in sources:
        lines = []
        with open(path, encoding='utf-8-sig') as fh:
            for raw in fh:
                line = strip_comment(raw).strip()
                if line:
                    lines.extend(q for q in (p.strip() for p in split_top(line, ';')) if q)
        modules[path] = lines

    lint = Lint()
    for lines in modules.values():
        lint.scan_objects(lines)
    for lines in modules.values():
        lint.scan_declarations(lines, '')
    for path, lines in modules.items():
        for kind, name, params, body in iter_blocks(lines):
            if kind != 'function':
                continue
            if name in lint.functions:
                lint.error(path, f"повторное объявление функции '{name}'")
            lint.functions[name] = params
    for path, lines in modules.items():
        for kind, name, params, body in iter_blocks(lines):
            lint.check_function(path, name, params, body)

    return lint.errors


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'game'
    scripts = os.path.join(root, 'scripts')
    if not os.path.isdir(scripts):
        scripts = root
    errors = lint_dir(scripts)
    if errors:
        print(f'ds_lint: {len(errors)} problem(s)')
        for e in errors:
            print(' -', e)
        return 1
    print('ds_lint: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
