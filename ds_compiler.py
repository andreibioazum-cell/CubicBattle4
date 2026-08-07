#!/usr/bin/env python3
"""
DimScript Compiler - LuaMC Syntax
Поддерживает: class, fn, loop, delete, new, Screen.*, Draw.*, File.*, Vibrate
"""

import sys, os, re
from dataclasses import dataclass
from typing import List, Dict, Optional, Tuple

# ============================================
# КЛАССЫ
# ============================================

@dataclass
class Field:
    name: str; type: str; default: Optional[str] = None

@dataclass
class ClassDef:
    name: str; fields: List[Field]

@dataclass
class VarDecl:
    name: str; type: str; value: Optional[str] = None

@dataclass
class FunctionDef:
    name: str; params: List[Tuple[str, str]]; body: List[str]; is_main: bool = False

# ============================================
# КОМПИЛЯТОР
# ============================================

class DimScriptCompiler:
    def __init__(self):
        self.classes = {}
        self.vars = {}
        self.functions = {}
        self.main_func = None
        self.output = []
        self.errors = 0
        self.source_lines = []
        self.indent = 0
        self.temp = 0

    # ---------- ПАРСИНГ ----------

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
            if line.startswith('class '):
                i = self.parse_class(i)
            elif re.match(r'^(int|num|bool|byte\*|size|col|\w+\*)\s+', line):
                i = self.parse_var(i)
            elif line.startswith('fn '):
                i = self.parse_function(i)
            elif 'Main(' in line:
                i = self.parse_main(i)
            else:
                i += 1
        return self.errors == 0

    def parse_class(self, i: int) -> int:
        name = self.source_lines[i].split()[1]
        cls = ClassDef(name, [])
        i += 1
        while i < len(self.source_lines):
            line = self.source_lines[i].strip()
            if line == '}':
                break
            if line.endswith(';'):
                line = line[:-1]
            parts = line.split()
            if len(parts) >= 2:
                ftype, fname = parts[0], parts[1].split('=')[0].strip()
                default = parts[1].split('=')[1].strip() if '=' in parts[1] else None
                cls.fields.append(Field(fname, ftype, default))
            i += 1
        self.classes[name] = cls
        return i + 1

    def parse_var(self, i: int) -> int:
        line = self.source_lines[i].strip()
        if line.endswith(';'): line = line[:-1]
        parts = line.split()
        if len(parts) >= 2:
            vtype, rest = parts[0], ' '.join(parts[1:])
            vname = rest.split('=')[0].strip()
            value = rest.split('=')[1].strip() if '=' in rest else None
            self.vars[vname] = VarDecl(vname, vtype, value)
        return i + 1

    def parse_function(self, i: int) -> int:
        line = self.source_lines[i]
        rest = line[2:].strip()
        name = rest.split('(')[0].strip()
        params_str = rest.split('(')[1].split(')')[0]
        params = []
        if params_str:
            for p in params_str.split(','):
                p = p.strip()
                if p:
                    ptype, pname = p.split()
                    params.append((ptype, pname))
        body = []
        i += 1
        while i < len(self.source_lines) and self.source_lines[i] != '}':
            body.append(self.source_lines[i])
            i += 1
        self.functions[name] = FunctionDef(name, params, body)
        return i + 1

    def parse_main(self, i: int) -> int:
        body = []
        i += 1
        while i < len(self.source_lines) and self.source_lines[i] != '}':
            body.append(self.source_lines[i])
            i += 1
        self.main_func = FunctionDef("Main", [], body, True)
        return i + 1

    # ---------- ГЕНЕРАЦИЯ ----------

    def generate(self):
        self.emit('#include "runtime.h"')
        self.emit('#include <math.h>')
        self.emit('#include <string.h>')
        self.emit('')
        
        # Структуры
        for cls in self.classes.values():
            self.emit(f'typedef struct {{')
            for f in cls.fields:
                self.emit(f'    {self.c_type(f.type)} {f.name};', 1)
            self.emit(f'}} {cls.name};')
            self.emit('')
        
        # Глобальные переменные
        for v in self.vars.values():
            val = self.c_value(v.value) if v.value else ('NULL' if '*' in v.type else '0')
            self.emit(f'{self.c_type(v.type)} {v.name} = {val};')
        if self.vars: self.emit('')
        
        # Прототипы
        for f in self.functions.values():
            params = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in f.params)
            self.emit(f'static void ds_fn_{f.name}({params});')
        if self.main_func:
            self.emit('int ds_main(void);')
        self.emit('')
        
        # Функции
        for f in self.functions.values():
            self.emit_function(f)
        
        # Main
        if self.main_func:
            self.emit_main()
        
        # Хуки
        self.emit_hooks()

    def emit_function(self, f: FunctionDef):
        params = ', '.join(f'{self.c_type(p[0])} {p[1]}' for p in f.params)
        self.emit(f'static void ds_fn_{f.name}({params}) {{')
        for line in f.body:
            self.compile_line(line)
        self.emit('}')
        self.emit('')

    def emit_main(self):
        self.emit('int ds_main(void) {')
        for line in self.main_func.body:
            self.compile_line(line)
        self.emit('    return 0;')
        self.emit('}')
        self.emit('')

    # ---------- КОМПИЛЯЦИЯ СТРОК ----------

    def compile_line(self, line: str):
        line = line.strip()
        if not line: return
        
        # if, loop, while, for
        if line.startswith('if '):
            self.compile_if(line)
        elif line.startswith('loop '):
            cond = line[5:].strip().strip('()').rstrip('{')
            self.emit(f'while ({cond}) {{')
        elif line.startswith('while '):
            cond = line[6:].strip().strip('()').rstrip('{')
            self.emit(f'while ({cond}) {{')
        elif line.startswith('for '):
            self.compile_for(line)
        elif ' = new ' in line:
            self.compile_new(line)
        elif line.startswith('delete '):
            var = line[7:].strip().rstrip(';')
            self.emit(f'free({var}); {var} = NULL;')
        elif 'Screen.' in line:
            self.compile_screen(line)
        elif 'Draw.' in line:
            self.compile_draw(line)
        elif 'Touch.' in line:
            self.compile_touch(line)
        elif 'Vibrate(' in line:
            ms = re.search(r'\d+', line)
            self.emit(f'// Vibrate({ms.group() if ms else 0}) - add to runtime')
        elif 'File.Read' in line:
            self.compile_file_read(line)
        elif 'File.Write' in line:
            self.compile_file_write(line)
        elif ' = ' in line or '+=' in line or '-=' in line or '*=' in line or '/=' in line:
            self.compile_assign(line)
        elif line.startswith('return '):
            val = line[7:].rstrip(';')
            self.emit(f'return {val};')
        elif line == '}':
            self.emit('}')
        elif '(' in line and ')' in line:
            self.compile_call(line)
        else:
            self.emit(f'{line};')

    def compile_if(self, line: str):
        cond = line[2:].strip().rstrip(';')
        if ' return;' in cond:
            c, _ = cond.split(' return;', 1)
            self.emit(f'if ({c}) {{ return; }}')
        else:
            self.emit(f'if ({cond}) {{')

    def compile_for(self, line: str):
        m = re.search(r'for\s*\(\s*(.+?);\s*(.+?);\s*(.+?)\s*\)', line)
        if m:
            init, cond, inc = m.groups()
            self.emit('{')
            self.emit(f'    {init};')
            self.emit(f'    while ({cond}) {{')
            self.emit(f'        // body')
            self.emit(f'        {inc};')
            self.emit('    }')
            self.emit('}')

    def compile_new(self, line: str):
        var, cls = line.split(' = new ')
        var = var.strip()
        cls = cls.split('(')[0].strip()
        if cls in self.classes:
            self.emit(f'{cls}* {var} = ({cls}*)calloc(1, sizeof({cls}));')
            for f in self.classes[cls].fields:
                if f.default:
                    self.emit(f'if ({var}) {var}->{f.name} = {self.c_value(f.default)};')
        else:
            self.emit(f'{var} = NULL; // new {cls} not found')

    def compile_screen(self, line: str):
        if 'Screen.Title' in line:
            t = re.search(r'"([^"]+)"', line)
            if t: self.emit(f'// Screen title: {t.group(1)}')
        elif 'Screen.Size' in line:
            nums = re.findall(r'\d+', line)
            if len(nums) >= 2:
                self.emit(f'// Screen size: {nums[0]}x{nums[1]}')
        elif 'Screen.FPS' in line:
            nums = re.findall(r'\d+', line)
            if nums: self.emit(f'// FPS: {nums[0]}')
        elif 'Screen.Flip' in line:
            self.emit('// Screen flip')
        elif 'Screen.Active' in line:
            self.emit('1 // Screen always active')

    def compile_draw(self, line: str):
        if 'Draw.Clear' in line:
            self.emit('cls(0x1A1A26);')
        elif 'Draw.Circle' in line:
            args = [a.strip() for a in line[line.find('(')+1:line.rfind(')')].split(',')]
            if len(args) >= 4:
                mode, x, y, r = args[0].strip('"'), args[1], args[2], args[3]
                self.emit(f'circle({x}, {y}, {r}, 0x33CC33);')
        elif 'Draw.Rect' in line:
            self.emit('// rect not implemented')
        elif 'Draw.Text' in line:
            m = re.search(r'"([^"]+)"', line)
            nums = re.findall(r'[\d.]+', line)
            if m and len(nums) >= 2:
                self.emit(f'text("{m.group(1)}", {nums[0]}, {nums[1]}, 0xFFFFFF);')
        elif 'Draw.Color' in line:
            self.emit('// Draw.Color not needed in this runtime')
        elif 'Draw.Target' in line:
            self.emit('// Draw.Target not needed')

    def compile_touch(self, line: str):
        if 'Touch.Poll' in line:
            self.emit('// Touch polling handled by runtime')
        elif 'Touch.Down' in line:
            self.emit('0 // Touch down constant')

    def compile_file_read(self, line: str):
        m = re.search(r'"([^"]+)"', line)
        if m:
            self.emit(f'// File.Read("{m.group(1)}") not implemented yet')
            self.emit('int val = 0;')

    def compile_file_write(self, line: str):
        m = re.search(r'"([^"]+)"', line)
        if m:
            self.emit(f'// File.Write("{m.group(1)}", val) not implemented yet')

    def compile_assign(self, line: str):
        if line.endswith(';'): line = line[:-1]
        for op in ('+=', '-=', '*=', '/='):
            if op in line:
                l, r = line.split(op, 1)
                self.emit(f'{l.strip()} = {l.strip()} {op[0]} {r.strip()};')
                return
        if '=' in line:
            l, r = line.split('=', 1)
            self.emit(f'{l.strip()} = {r.strip()};')

    def compile_call(self, line: str):
        name = line.split('(')[0].strip()
        args = line[line.find('(')+1:line.rfind(')')]
        if name in self.functions:
            self.emit(f'ds_fn_{name}({args});')
        elif name in ('UpdateParts', 'SpawnPart', 'ClearParts', 'Update', 'Draw', 'DrawBall', 'DrawParts', 'DrawUI', 'DrawPause', 'OnTouch', 'OnSwipe', 'OnPinch', 'OnBack', 'LoadBest', 'SaveBest'):
            self.emit(f'ds_fn_{name}({args});')
        else:
            self.emit(f'{line}')

    # ---------- ВСПОМОГАТЕЛЬНЫЕ ----------

    def c_type(self, t: str) -> str:
        m = {'int': 'int', 'num': 'double', 'bool': 'int', 'byte*': 'unsigned char*', 
             'size': 'size_t', 'col': 'uint32_t'}
        if t.endswith('*'):
            base = t[:-1]
            if base in self.classes: return f'{base}*'
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
        if 'Update' in self.functions:
            self.emit('    // Update called from main loop')
        self.emit('}')
        self.emit('')
        self.emit('void draw(Buffer *buffer) {')
        self.emit('    (void)buffer;')
        self.emit('}')
        self.emit('')
        self.emit('void touch(float x, float y, int action) {')
        self.emit('    (void)x; (void)y; (void)action;')
        self.emit('}')

    # ---------- ЗАПУСК ----------

    def compile(self, sources: List[str], output: str) -> bool:
        if not self.parse(sources): return False
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
