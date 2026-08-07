#!/usr/bin/env python3
"""
DimScript Compiler - Простой компилятор для синтаксиса LuaMC
"""

import sys, os, re
from typing import List

class DimScriptCompiler:
    def __init__(self):
        self.vars = {}
        self.functions = {}
        self.main_body = []
        self.output = []
        self.errors = 0
        self.source_lines = []
        self.in_function = False
        self.current_function = None

    def parse(self, paths: List[str]) -> bool:
        for path in paths:
            try:
                with open(path, 'r') as f:
                    for line in f:
                        line = line.strip()
                        if line and not line.startswith('//'):
                            self.source_lines.append(line)
            except Exception as e:
                print(f"Error: {e}", file=sys.stderr)
                return False
        
        i = 0
        while i < len(self.source_lines):
            line = self.source_lines[i]
            
            if re.match(r'^(int|num|bool|byte\*|size|col|\w+\*)\s+', line):
                self.parse_var(line)
                i += 1
            elif line.startswith('fn '):
                i = self.parse_function(i)
            elif 'Main(' in line:
                i = self.parse_main(i)
            else:
                i += 1
        
        return self.errors == 0

    def parse_var(self, line: str):
        if line.endswith(';'): line = line[:-1]
        parts = line.split()
        if len(parts) >= 2:
            vtype, rest = parts[0], ' '.join(parts[1:])
            vname = rest.split('=')[0].strip()
            value = rest.split('=')[1].strip() if '=' in rest else None
            self.vars[vname] = {'type': vtype, 'value': value}

    def parse_function(self, i: int) -> int:
        line = self.source_lines[i]
        rest = line[2:].strip()
        name = rest.split('(')[0].strip()
        params_str = rest.split('(')[1].split(')')[0]
        params = []
        if params_str.strip():
            for p in params_str.split(','):
                p = p.strip()
                if p:
                    parts = p.split()
                    if len(parts) == 2:
                        params.append((parts[0], parts[1]))
                    else:
                        params.append(('num', p))
        
        body = []
        i += 1
        while i < len(self.source_lines):
            line = self.source_lines[i]
            if line == '}':
                break
            body.append(line)
            i += 1
        
        self.functions[name] = {
            'params': params,
            'body': body
        }
        return i + 1

    def parse_main(self, i: int) -> int:
        body = []
        i += 1
        while i < len(self.source_lines):
            line = self.source_lines[i]
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
        for name, var in self.vars.items():
            vtype = self.c_type(var['type'])
            val = self.c_value(var['value']) if var['value'] else ('NULL' if '*' in var['type'] else '0')
            self.emit(f'{vtype} {name} = {val};')
        if self.vars:
            self.emit('')
        
        # Прототипы функций
        for name, func in self.functions.items():
            params = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in func['params'])
            self.emit(f'static void ds_fn_{name}({params});')
        self.emit('')
        
        # Тела функций
        for name, func in self.functions.items():
            self.emit_function(name, func)
        
        # Main
        self.emit_main()
        
        # Хуки
        self.emit_hooks()

    def emit_function(self, name: str, func: dict):
        params = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in func['params'])
        self.emit(f'static void ds_fn_{name}({params}) {{')
        for line in func['body']:
            self.compile_line(line)
        self.emit('}')
        self.emit('')

    def emit_main(self):
        self.emit('int ds_main(void) {')
        for line in self.main_body:
            self.compile_line(line)
        self.emit('    return 0;')
        self.emit('}')
        self.emit('')

    def compile_line(self, line: str):
        line = line.strip()
        if not line:
            return
        
        if line.startswith('if '):
            cond = line[2:].strip().rstrip(';')
            if cond.endswith('{'):
                cond = cond[:-1]
            self.emit(f'if ({cond}) {{')
        
        elif line.startswith('loop '):
            cond = line[5:].strip().strip('()').rstrip('{')
            self.emit(f'while ({cond}) {{')
        
        elif line.startswith('while '):
            cond = line[6:].strip().strip('()').rstrip('{')
            self.emit(f'while ({cond}) {{')
        
        elif line.startswith('for '):
            m = re.search(r'for\s*\(\s*(.+?);\s*(.+?);\s*(.+?)\s*\)', line)
            if m:
                init, cond, inc = m.groups()
                self.emit('{')
                self.emit(f'    {init};')
                self.emit(f'    while ({cond}) {{')
                self.emit(f'        {inc};')
                self.emit('    }')
                self.emit('}')
        
        elif ' = new ' in line:
            var, cls = line.split(' = new ')
            var = var.strip()
            cls = cls.split('(')[0].strip()
            self.emit(f'{cls}* {var} = ({cls}*)calloc(1, sizeof({cls}));')
        
        elif line.startswith('delete '):
            var = line[7:].strip().rstrip(';')
            self.emit(f'free({var}); {var} = NULL;')
        
        elif line.startswith('return '):
            val = line[7:].rstrip(';')
            self.emit(f'return {val};')
        
        elif line == '}':
            self.emit('}')
        
        elif '(' in line and ')' in line and not ('=' in line or '+=' in line):
            name = line.split('(')[0].strip()
            args = line[line.find('(')+1:line.rfind(')')]
            if name in self.functions:
                self.emit(f'ds_fn_{name}({args});')
            else:
                self.emit(f'{line}')
        
        elif '=' in line or '+=' in line or '-=' in line or '*=' in line or '/=' in line:
            self.compile_assign(line)
        
        else:
            self.emit(f'{line};')

    def compile_assign(self, line: str):
        if line.endswith(';'):
            line = line[:-1]
        for op in ('+=', '-=', '*=', '/='):
            if op in line:
                l, r = line.split(op, 1)
                self.emit(f'{l.strip()} = {l.strip()} {op[0]} {r.strip()};')
                return
        if '=' in line:
            l, r = line.split('=', 1)
            self.emit(f'{l.strip()} = {r.strip()};')

    def c_type(self, t: str) -> str:
        m = {'int': 'int', 'num': 'double', 'bool': 'int', 'byte*': 'unsigned char*', 
             'size': 'size_t', 'col': 'uint32_t'}
        if t.endswith('*'):
            return t
        return m.get(t, t)

    def c_value(self, v: str) -> str:
        if v == 'null': return 'NULL'
        if v == 'true': return '1'
        if v == 'false': return '0'
        return v

    def emit(self, line: str = '', indent: int = 0):
        self.output.append(('    ' * indent) + line)

    def emit_hooks(self):
        self.emit('')
        self.emit('void init(AAssetManager *assets) {')
        self.emit('    (void)assets;')
        self.emit('    ds_main();')
        self.emit('}')
        self.emit('')
        self.emit('void update(void) {')
        if 'update' in self.functions:
            self.emit('    ds_fn_update();')
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
            # Проверяем параметры функции touch
            func = self.functions['touch']
            if len(func['params']) >= 3:
                self.emit('    ds_fn_touch(x, y, action);')
            else:
                self.emit('    ds_fn_touch();')
        self.emit('}')

    def compile(self, sources: List[str], output: str) -> bool:
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
        print("Usage: python ds_compiler.py file.ds -o output.c", file=sys.stderr)
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
        print("Error: no input files", file=sys.stderr)
        sys.exit(2)
    
    comp = DimScriptCompiler()
    sys.exit(0 if comp.compile(sources, output) else 1)

if __name__ == '__main__':
    main()
