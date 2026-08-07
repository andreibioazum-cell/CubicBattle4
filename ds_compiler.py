#!/usr/bin/env python3
"""
DimScript Compiler - Генерирует C-код из .ds файлов
"""

import sys, re

class DimScriptCompiler:
    def __init__(self):
        self.vars = {}
        self.functions = {}
        self.main_body = []
        self.output = []
        self.src = []
        self.errors = 0
        self.in_function = False
        self.current_function = None

    def parse(self, paths):
        for path in paths:
            try:
                with open(path, 'r') as f:
                    for line in f:
                        line = line.strip()
                        if line and not line.startswith('//'):
                            self.src.append(line)
            except Exception as e:
                print(f"Error: {e}")
                return False
        
        i = 0
        while i < len(self.src):
            line = self.src[i]
            if re.match(r'^(int|num|bool|byte\*|size|col|\w+\*)\s+', line):
                self.parse_var(line)
                i += 1
            elif line.startswith('fn '):
                i = self.parse_func(i)
            elif 'Main(' in line:
                i = self.parse_main(i)
            else:
                i += 1
        return True

    def parse_var(self, line):
        line = line.replace(';', '')
        parts = line.split()
        if len(parts) >= 2:
            vtype, rest = parts[0], ' '.join(parts[1:])
            vname = rest.split('=')[0].strip()
            value = rest.split('=')[1].strip() if '=' in rest else None
            self.vars[vname] = (vtype, value)

    def parse_func(self, i):
        line = self.src[i]
        rest = line[2:].strip()
        name = rest.split('(')[0].strip()
        params_str = rest.split('(')[1].split(')')[0]
        params = []
        if params_str.strip():
            for p in params_str.split(','):
                p = p.strip()
                if p:
                    pts = p.split()
                    params.append((pts[0] if len(pts) > 1 else 'num', pts[-1] if pts else 'p'))
        
        body = []
        i += 1
        while i < len(self.src):
            line = self.src[i]
            if line == '}':
                break
            body.append(line)
            i += 1
        
        self.functions[name] = (params, body)
        return i + 1

    def parse_main(self, i):
        body = []
        i += 1
        while i < len(self.src):
            line = self.src[i]
            if line == '}':
                break
            body.append(line)
            i += 1
        self.main_body = body
        return i + 1

    def generate(self):
        self.output = []
        self.emit('#include "runtime.h"')
        self.emit('#include <math.h>')
        self.emit('#include <string.h>')
        self.emit('')
        
        # Глобальные переменные
        for name, (vtype, val) in self.vars.items():
            ct = self.c_type(vtype)
            cv = val if val else ('NULL' if '*' in vtype else '0')
            self.emit(f'{ct} {name} = {cv};')
        if self.vars:
            self.emit('')
        
        # Прототипы
        for name, (params, body) in self.functions.items():
            ps = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in params)
            self.emit(f'static void ds_fn_{name}({ps});')
        self.emit('')
        
        # Тела функций
        for name, (params, body) in self.functions.items():
            ps = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in params)
            self.emit(f'static void ds_fn_{name}({ps}) {{')
            for line in body:
                self.compile_line(line, inside_func=True)
            self.emit('}')
            self.emit('')
        
        # Main
        self.emit('int ds_main(void) {')
        for line in self.main_body:
            self.compile_line(line, inside_func=True)
        self.emit('    return 0;')
        self.emit('}')
        self.emit('')
        
        # Хуки
        self.emit('void init(AAssetManager *assets) {')
        self.emit('    (void)assets;')
        self.emit('    ds_main();')
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

    def compile_line(self, line, inside_func=False):
        line = line.strip()
        if not line:
            return
        
        # Обработка объявления локальной переменной: num dx = touch_x - joy_x;
        if inside_func and re.match(r'^(num|int|bool)\s+[a-zA-Z_][a-zA-Z0-9_]*', line):
            # Заменяем num на double
            line = re.sub(r'^num\s+', 'double ', line)
            line = re.sub(r'^int\s+', 'int ', line)
            line = re.sub(r'^bool\s+', 'int ', line)
            # Добавляем отступ
            self.emit(f'    {line}')
            return
        
        if line.startswith('if '):
            cond = line[2:].strip()
            if cond.endswith('{'): cond = cond[:-1]
            self.emit(f'    if ({cond}) {{')
        elif line.startswith('loop '):
            cond = line[5:].strip()
            if cond.endswith('{'): cond = cond[:-1]
            cond = cond.strip('()')
            self.emit(f'    while ({cond}) {{')
        elif line.startswith('while '):
            cond = line[6:].strip()
            if cond.endswith('{'): cond = cond[:-1]
            cond = cond.strip('()')
            self.emit(f'    while ({cond}) {{')
        elif line == '}':
            self.emit('    }')
        elif line.startswith('return '):
            val = line[7:].strip()
            self.emit(f'    return {val};')
        elif ' = new ' in line:
            var, cls = line.split(' = new ')
            var = var.strip()
            cls = cls.split('(')[0].strip()
            self.emit(f'    {cls}* {var} = ({cls}*)calloc(1, sizeof({cls}));')
        elif line.startswith('delete '):
            var = line[7:].strip()
            self.emit(f'    free({var}); {var} = NULL;')
        elif '(' in line and ')' in line and not any(op in line for op in ['=', '+=', '-=', '*=', '/=']):
            name = line.split('(')[0].strip()
            args = line[line.find('(')+1:line.rfind(')')]
            if name in self.functions:
                self.emit(f'    ds_fn_{name}({args});')
            else:
                self.emit(f'    {line}')
        elif any(op in line for op in ['=', '+=', '-=', '*=', '/=']):
            self.compile_assign(line, inside_func)
        else:
            if inside_func:
                self.emit(f'    {line};')
            else:
                self.emit(f'{line};')

    def compile_assign(self, line, inside_func=False):
        line = line.replace(';', '')
        for op in ('+=', '-=', '*=', '/='):
            if op in line:
                l, r = line.split(op, 1)
                self.emit(f'    {l.strip()} = {l.strip()} {op[0]} {r.strip()};')
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

    def emit(self, line, indent=0):
        if indent:
            self.output.append('    ' * indent + line)
        else:
            self.output.append(line)

    def compile(self, sources, output):
        if not self.parse(sources):
            return False
        self.generate()
        if self.errors == 0:
            with open(output, 'w') as f:
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
            output = sys.argv[i+1] if i+1 < len(sys.argv) else 'game/game.c'
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
