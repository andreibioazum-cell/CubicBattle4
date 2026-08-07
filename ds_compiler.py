#!/usr/bin/env python3
"""
DimScript Compiler - Исправленная версия
- Корректно обрабатывает вложенные блоки { } (раньше ломался на первом '}' внутри функции)
- Добавляет недостающие ';' для вызовов функций и локальных переменных
- Не выносит локальные переменные из функций в глобальные
"""

import sys
import re


class DimScriptCompiler:
    def __init__(self):
        self.vars = {}
        self.functions = {}
        self.main_body = []
        self.output = []
        self.src = []
        self.errors = 0

    def parse(self, paths):
        for path in paths:
            try:
                with open(path, 'r', encoding='utf-8') as f:
                    for raw in f:
                        line = raw.strip()
                        # пропуск пустых и комментариев
                        if not line or line.startswith('//'):
                            continue
                        # убираем inline комментарии
                        if '//' in line:
                            line = line.split('//', 1)[0].strip()
                            if not line:
                                continue
                        self.src.append(line)
            except Exception as e:
                print(f"Error: {e}")
                return False

        i = 0
        while i < len(self.src):
            line = self.src[i]
            if re.match(r'^(int|num|bool|byte\*|size|col|\w+\*)\s+', line):
                # глобальная переменная только если мы НЕ внутри функции
                self.parse_var(line)
                i += 1
            elif line.startswith('fn '):
                i = self.parse_func(i)
            elif 'Main(' in line:
                i = self.parse_main(i)
            else:
                # неизвестная строка на верхнем уровне — пропускаем
                i += 1
        return True

    def parse_var(self, line):
        line = line.replace(';', '').strip()
        parts = line.split()
        if len(parts) >= 2:
            vtype, rest = parts[0], ' '.join(parts[1:])
            vname = rest.split('=')[0].strip()
            value = rest.split('=')[1].strip() if '=' in rest else None
            # защита от перезаписи: если уже есть — не перезаписываем, если это локальная внутри функции
            # но на этапе parse_vars мы вызываемся только для глобальных, так что можно смело писать
            self.vars[vname] = (vtype, value)

    def parse_func(self, i):
        line = self.src[i]
        # fn name(params) {
        rest = line[2:].strip()
        # убрать возможный '{' в конце
        if rest.endswith('{'):
            rest = rest[:-1].strip()
        name = rest.split('(')[0].strip()
        params_str = ''
        if '(' in rest and ')' in rest:
            params_str = rest.split('(', 1)[1].rsplit(')', 1)[0]

        params = []
        if params_str.strip():
            for p in params_str.split(','):
                p = p.strip()
                if not p:
                    continue
                pts = p.split()
                # если указан тип: "num x", иначе просто "x" -> считаем num
                if len(pts) > 1:
                    params.append((pts[0], pts[-1]))
                else:
                    params.append(('num', pts[0]))

        body = []
        # считаем вложенность фигурных скобок: уже открыли 1 на строке fn
        depth = 1
        i += 1
        while i < len(self.src):
            cur = self.src[i]
            # подсчёт глубины до добавления в body, чтобы правильно обработать закрывающую '}'
            # если строка содержит '{' — увеличиваем, если '}' — уменьшаем
            # важно: строки типа "} else {" — обрабатываем
            open_cnt = cur.count('{')
            close_cnt = cur.count('}')

            if cur == '}' and depth == 1 and open_cnt == 0 and close_cnt == 1:
                # закрытие самой функции
                depth -= 1
                break
            else:
                # для вложенных блоков
                # добавляем строку в body (даже если это '}')
                body.append(cur)
                depth += open_cnt - close_cnt
                if depth <= 0:
                    # убрали последнюю добавленную '}' которая была закрытием функции — не должна быть в теле
                    # но наш алгоритм выше уже обработал случай одиночной '}'
                    # для случая когда функция закрывается строкой с несколькими символами — редко, но обработаем
                    if body and body[-1] == '}':
                        body.pop()
                    break
            i += 1

        self.functions[name] = (params, body)
        return i + 1

    def parse_main(self, i):
        body = []
        depth = 1
        i += 1
        while i < len(self.src):
            cur = self.src[i]
            open_cnt = cur.count('{')
            close_cnt = cur.count('}')
            if cur == '}' and depth == 1:
                depth -= 1
                break
            body.append(cur)
            depth += open_cnt - close_cnt
            if depth <= 0:
                if body and body[-1] == '}':
                    body.pop()
                break
            i += 1
        self.main_body = body
        return i + 1

    def generate(self):
        self.output = []
        self.emit('#include "runtime.h"')
        self.emit('#include <math.h>')
        self.emit('')

        # Глобальные переменные
        for name, (vtype, val) in self.vars.items():
            ct = self.c_type(vtype)
            cv = val if val is not None and val != '' else ('NULL' if '*' in vtype else '0')
            self.emit(f'{ct} {name} = {cv};')
        if self.vars:
            self.emit('')

        # Прототипы
        for name, (params, body) in self.functions.items():
            ps = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in params)
            if not ps:
                ps = 'void'
            self.emit(f'static void ds_fn_{name}({ps});')
        if self.functions:
            self.emit('')

        # Тела функций
        for name, (params, body) in self.functions.items():
            ps = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in params)
            if not ps:
                ps = 'void'
            self.emit(f'static void ds_fn_{name}({ps}) {{')
            for line in body:
                self.compile_line(line)
            self.emit('}')
            self.emit('')

        # Main
        self.emit('int ds_main(void) {')
        for line in self.main_body:
            self.compile_line(line)
        self.emit('    return 0;')
        self.emit('}')
        self.emit('')

        # Хуки
        self.emit('void init(AAssetManager *assets) {')
        self.emit('    (void)assets;')
        self.emit('    ds_main();')
        if 'init' in self.functions:
            self.emit('    ds_fn_init();')
        self.emit('}')
        self.emit('')
        self.emit('void update(void) {')
        if 'update' in self.functions:
            self.emit('    ds_fn_update();')
        if 'update_touch' in self.functions:
            self.emit('    ds_fn_update_touch();')
        self.emit('}')
        self.emit('')
        self.emit('void draw(Buffer *buffer) {')
        self.emit('    (void)buffer;')
        if 'draw' in self.functions:
            self.emit('    ds_fn_draw();')
        self.emit('}')
        self.emit('')
        self.emit('void touch(float x, float y, int action) {')
        if 'touch' in self.functions:
            self.emit('    ds_fn_touch((double)x, (double)y, (double)action);')
        self.emit('}')

    def compile_line(self, line):
        line = line.strip()
        if not line:
            return

        # Локальная переменная: num/dx, int, bool
        if re.match(r'^(num|int|bool)\s+[a-zA-Z_][a-zA-Z0-9_]*', line):
            c_line = re.sub(r'^num\s+', 'double ', line)
            c_line = re.sub(r'^int\s+', 'int ', c_line)
            c_line = re.sub(r'^bool\s+', 'int ', c_line)
            # гарантируем точку с запятой
            if not c_line.rstrip().endswith(';'):
                c_line = c_line.rstrip() + ';'
            self.emit(f'    {c_line}')
            return

        # if
        if line.startswith('if '):
            cond = line[2:].strip()
            # убрать trailing '{'
            if cond.endswith('{'):
                cond = cond[:-1].strip()
            self.emit(f'    if ({cond}) {{')
            return

        # loop
        if line.startswith('loop '):
            cond = line[5:].strip()
            if cond.endswith('{'):
                cond = cond[:-1].strip()
            cond = cond.strip('()')
            self.emit(f'    while ({cond}) {{')
            return

        # while
        if line.startswith('while '):
            cond = line[6:].strip()
            if cond.endswith('{'):
                cond = cond[:-1].strip()
            cond = cond.strip('()')
            self.emit(f'    while ({cond}) {{')
            return

        # return
        if line.startswith('return'):
            # return; или return value;
            ret = line[6:].strip()
            ret = ret.rstrip(';')
            if ret:
                self.emit(f'    return {ret};')
            else:
                self.emit('    return;')
            return

        # new
        if ' = new ' in line:
            var, cls = line.split(' = new ', 1)
            var = var.strip()
            cls = cls.split('(')[0].strip()
            self.emit(f'    {cls}* {var} = ({cls}*)calloc(1, sizeof({cls}));')
            return

        # delete
        if line.startswith('delete '):
            var = line[7:].strip().rstrip(';')
            self.emit(f'    free({var}); {var} = NULL;')
            return

        # закрывающая скобка
        if line == '}':
            self.emit('    }')
            return
        # обработка строки типа "} else {" — редко, но поддержим
        if line.startswith('}'):
            # например "} else {"
            rest = line[1:].strip()
            self.emit('    }')
            if rest:
                # рекурсивно обработать остаток
                self.compile_line(rest)
            return
        if line.endswith('{'):
            # неожиданный блок, просто пробрасываем
            self.emit(f'    {line}')
            return

        # Вызов функции или присваивание
        if '(' in line and ')' in line:
            # если это присваивание с вызовом: x = foo()
            if any(op in line for op in ('=', '+=', '-=', '*=', '/=')):
                # проверим, что '=' не внутри скобок условия — простое эвристика: '=' до '(' ?
                # для безопасности идём в compile_assign
                if '=' in line.split('(')[0]:
                    self.compile_assign(line)
                    return
            # обычный вызов
            name = line.split('(')[0].strip().split()[-1]  # последний токен перед '('
            args = line[line.find('(') + 1: line.rfind(')')]
            if name in self.functions:
                self.emit(f'    ds_fn_{name}({args});')
            else:
                # внешние функции типа cls, rect, circle, ring, sqrt и т.д.
                clean = line.rstrip()
                if not clean.endswith(';'):
                    clean += ';'
                self.emit(f'    {clean}')
            return

        # Присваивание
        if any(op in line for op in ('+=', '-=', '*=', '/=', '=')):
            self.compile_assign(line)
            return

        # Всё остальное
        clean = line.rstrip()
        if not clean.endswith(';') and not clean.endswith('}'):
            clean += ';'
        self.emit(f'    {clean}')

    def compile_assign(self, line):
        line = line.replace(';', '').strip()
        for op in ('+=', '-=', '*=', '/='):
            if op in line:
                l, r = line.split(op, 1)
                self.emit(f'    {l.strip()} {op} {r.strip()};')
                return
        if '=' in line:
            l, r = line.split('=', 1)
            self.emit(f'    {l.strip()} = {r.strip()};')

    def c_type(self, t):
        m = {'int': 'int', 'num': 'double', 'bool': 'int', 'byte*': 'unsigned char*',
             'size': 'size_t', 'col': 'uint32_t'}
        if t.endswith('*'):
            return t
        return m.get(t, t)

    def emit(self, line):
        self.output.append(line)

    def compile(self, sources, output):
        if not self.parse(sources):
            return False
        self.generate()
        if self.errors == 0:
            with open(output, 'w', encoding='utf-8') as f:
                f.write('\n'.join(self.output))
            return True
        return False


def main():
    if len(sys.argv) < 2:
        print("Usage: python ds_compiler.py file.ds -o output.c")
        sys.exit(2)

    output = 'game/game.c'
    sources = []
    i = 1
    while i < len(sys.argv):
        if sys.argv[i] in ('-o', '--output'):
            output = sys.argv[i + 1] if i + 1 < len(sys.argv) else 'game/game.c'
            i += 2
        else:
            sources.append(sys.argv[i])
            i += 1

    if not sources:
        print("Error: no input files")
        sys.exit(2)

    comp = DimScriptCompiler()
    sys.exit(0 if comp.compile(sources, output) else 1)


if __name__ == '__main__':
    main()
