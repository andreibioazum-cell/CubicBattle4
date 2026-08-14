#!/usr/bin/env python3
"""DimScript compiler: translates DimScript (.ds) to C99."""
import os, re, sys

TYPES = {'num': 'double', 'str': 'const char*', 'col': 'uint32_t', 'arr': 'DSArray*'}
BUILTINS = frozenset({
    'rect','roundrect','circle','ring','line','tex','text','text_scaled','text_ink_width','text_ink_height','text_ink_top','png_load',
    'sqrt','sin','cos','atan2','floor','rand','clamp','lerp','dist','now','str_len','str_eq',
    'net_connect','net_disconnect','net_publish','net_publish_bullet','net_status','net_slot','net_count',
    'net_player_online','net_player_x','net_player_y','net_player_angle','net_player_hp','net_player_alive',
    'net_player_bullet_active','net_player_bullet_x','net_player_bullet_y','net_player_bullet_dx','net_player_bullet_dy','net_player_bullet_shot','net_player_bullet_tr',
    'net_chat_send','net_chat_count','net_chat_text','net_chat_uid','net_chat_time',
    'keyboard_show','keyboard_hide','keyboard_get_text','keyboard_get_raw','keyboard_clear','keyboard_visible','keyboard_enter_pressed','keyboard_type','keyboard_backspace',
    'ds_log','console_count','console_line','console_type','console_clear',
    'arr_new','arr_push','arr_pop','arr_get','arr_set','arr_len','arr_clear','arr_free'
})
ENGINE_VARS = {'screen_w': 'num', 'screen_h': 'num', 'dt': 'num', 'joy': 'joy'}
STR_BUILTINS = frozenset({'console_line', 'keyboard_get_text', 'net_chat_text', 'net_chat_uid'})

_NAME = r'[A-Za-z_]\w*'
_FUNC_RE = re.compile(r'^function\s+(' + _NAME + r')(?:\s+(.*))?$')
_NUM_RE = re.compile(r'^(?:[-+]?\d+(?:\.\d+)?|0[xX][0-9a-fA-F]+)$')
_CALL_RE = re.compile(r'^(' + _NAME + r')(?:\s+(.*))?$')
_LHS_RE = re.compile(r'^(' + _NAME + r')(?:\.(' + _NAME + r'))?$')

def strip_comment(s):
    out, i, in_s = [], 0, False
    while i < len(s):
        c = s[i]
        if in_s:
            out.append(c)
            if c == '\\' and i + 1 < len(s): out.append(s[i+1]); i += 2; continue
            if c == '"': in_s = False
        elif c == '"': in_s = True; out.append(c)
        elif c == '/' and i + 1 < len(s) and s[i+1] == '/': break
        else: out.append(c)
        i += 1
    return ''.join(out)

def scan(t):
    n = len(t); depth, quoted, in_s, esc, lvl = [0]*n, [False]*n, False, False, 0
    for i, c in enumerate(t):
        quoted[i] = in_s
        if esc: esc = False; continue
        if in_s:
            if c == '\\': esc = True
            elif c == '"': in_s = False
            continue
        if c == '"': in_s = True
        elif c == '(': lvl += 1
        elif c == ')': lvl = max(0, lvl - 1)
        depth[i] = lvl
    return depth, quoted

def split_top(text, sep):
    depth, quoted = scan(text); parts, start = [], 0
    for i, c in enumerate(text):
        if depth[i] == 0 and not quoted[i] and c == sep:
            parts.append(text[start:i].strip()); start = i + 1
    parts.append(text[start:].strip())
    return parts

def find_assign(line):
    depth, quoted = scan(line)
    for i, c in enumerate(line):
        if not quoted[i] and not depth[i] and c == '=' and (i == 0 or line[i-1] not in '<>!') and (i + 1 >= len(line) or line[i+1] != '='):
            return i
    return -1

def used_outside_strings(text, name):
    pat = re.compile(r'\b' + re.escape(name) + r'\b'); _, quoted = scan(text)
    return any(not quoted[m.start()] for m in pat.finditer(text))

class DimScriptCompiler:
    def __init__(self):
        self.objects, self.vars, self.functions, self.func_ret = {}, {}, {}, {}
        self.top, self.lines, self.errors, self.output, self.indent, self.scope, self.blocks = [], [], 0, [], 0, {}, []

    def _error(self, msg): self.errors += 1; print(f"DimScript error: {msg}", file=sys.stderr)

    def _load(self, paths):
        for p in paths:
            try:
                with open(p, 'r', encoding='utf-8-sig') as f:
                    for raw in f:
                        line = strip_comment(raw).strip()
                        if not line: continue
                        if line[0] == 'c' and len(line) > 1 and line[1] in ' \t"':
                            self.lines.append(line); continue
                        for part in split_top(line, ';'):
                            if part.strip(): self.lines.append(part.strip())
            except OSError as e: self._error(f"cannot read '{p}': {e}"); return False
        return True

    def _decl_list(self, line):
        m = re.match(r'^(' + _NAME + r')\s+(.+)$', line)
        if not m: return None
        t, rest = m.group(1), m.group(2).strip()
        if t not in TYPES and t not in self.objects and t != 'joy': return None
        res = []
        for part in split_top(rest, ','):
            if not part.strip(): continue
            mm = re.match(r'^(' + _NAME + r')(?:\s*=\s*(.*))?$', part.strip())
            if not mm: return None
            res.append((t, mm.group(1), mm.group(2).strip() if mm.group(2) else None))
        return res or None

    def _decl_all(self, line):
        lst = self._decl_list(line)
        return lst if lst and all(t in TYPES or t in self.objects for t, _, _ in lst) else None

    def parse(self):
        i = 0
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end': self._error("unexpected 'end'"); i += 1
            elif line.startswith('object '): i = self._parse_object(i)
            elif line.startswith('function '): i = self._parse_function(i)
            elif self._decl_all(line): self._parse_global(line); i += 1
            else: self.top.append(line); i += 1
        return self.errors == 0

    def _parse_object(self, i):
        m = re.match(r'^object\s+(' + _NAME + r')(?:\s+(.+))?$', self.lines[i]); name = m.group(1); fields = {}
        j = i + 1
        while j < len(self.lines):
            line = self.lines[j]
            if line == 'end': self.objects[name] = fields; return j + 1
            lst = self._decl_all(line)
            if lst:
                for t, n, v in lst: fields[n] = (t, v)
            j += 1
        self._error(f"object '{name}' without end"); return j

    def _parse_function(self, i):
        m = _FUNC_RE.match(self.lines[i]); name = m.group(1)
        params = [tuple(p.split()) for p in split_top(m.group(2) or '', ',') if len(p.split()) == 2]
        body, j = self._collect_block(i + 1, f"function '{name}'")
        self.functions[name] = (params, body)
        if any(re.match(r'^return\s+\S', line) for line in body): self.func_ret[name] = 'num'
        return j

    def _infer_returns(self):
        for _ in range(3):
            for name, (params, body) in list(self.functions.items()):
                if name not in self.func_ret: continue
                saved, self.scope = self.scope, {pn: pt for pt, pn in params}; kind = 'num'
                for line in body:
                    lst = self._decl_all(line)
                    if lst:
                        for t, n, _ in lst:
                            if t in TYPES: self.scope[n] = t
                    elif line.startswith('return ') and self.expr_type(line[7:].strip()) == 'str':
                        kind = 'str'; break
                self.scope = saved; self.func_ret[name] = kind

    def _collect_block(self, i, what):
        depth, body = 0, []
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                if depth == 0: return body, i + 1
                depth -= 1
            elif line.startswith(('if ', 'loop ')): depth += 1
            elif line == 'else' or line.startswith('else if '):
                if depth == 0: return body, i + 1
            body.append(line); i += 1
        return body, i

    def _parse_global(self, line):
        for t, n, v in self._decl_all(line): self.vars[n] = (t, v)

    def c_type(self, t): return TYPES.get(t, t + ' *' if t in self.objects else 'double')
    def default_val(self, t): return 'NULL' if t == 'str' else '0'

    def expr_type(self, expr):
        expr = expr.strip()
        if expr.startswith('"') and expr.endswith('"'): return 'str'
        m = re.match(r'^(' + _NAME + r')\.(' + _NAME + r')$', expr)
        if m:
            holder, f = m.group(1), m.group(2)
            ot = self.scope.get(holder) or (self.vars[holder][0] if holder in self.vars else None)
            if self.objects.get(ot) and f in self.objects[ot]: return self.objects[ot][f][0]
        if expr in self.scope: return self.scope[expr]
        if expr in self.vars: return self.vars[expr][0]
        call = re.match(r'^(' + _NAME + r')\s*\(.*\)$', expr)
        if call:
            fn = call.group(1)
            if fn in self.func_ret: return self.func_ret[fn]
            if fn in STR_BUILTINS: return 'str'
        return ENGINE_VARS.get(expr, 'num')

    def expr(self, e):
        e = e.strip()
        if e in ('true', 'false'): return '1' if e == 'true' else '0'
        parts = split_top(e, '+')
        if len(parts) > 1 and any(self.expr_type(p) == 'str' for p in parts):
            out = self.as_str(parts[0])
            for p in parts[1:]: out = f'ds_concat({out}, {self.as_str(p)})'
            return out
        return self._fields(e)

    def _fields(self, e):
        names = [n for n, v in self.vars.items() if v[0] in self.objects] + [n for n, t in self.scope.items() if t in self.objects]
        for n in sorted(names, key=len, reverse=True):
            e = re.sub(r'\b' + re.escape(n) + r'\.(' + _NAME + r')', n + r'->\1', e)
        for fn in self.functions:
            e = re.sub(r'\b' + re.escape(fn) + r'\s*\(', f'ds_fn_{fn}(', e)
        return e

    def as_str(self, e): return self.expr(e) if self.expr_type(e) == 'str' else f'ds_num_to_string((double)({self.expr(e)}))'
    def _out(self, s): self.output.append('    ' * self.indent + s)
    def _emit(self, s): self.output.append(s)

    def _emit_line(self, line):
        if line.startswith('c ') or line.startswith('c\t'):
            raw = line[1:].strip()
            self._out(raw[1:-1].replace('\\"', '"').replace('\\\\', '\\') if raw.startswith('"') and raw.endswith('"') else (raw if raw.endswith((';', '{', '}')) else raw + ';'))
            return
        if line == 'end':
            if self.blocks: self.blocks.pop(); self.indent -= 1; self._out('}')
            return
        if line.startswith('if '):
            cond = re.sub(r'(\s+then:?|:)$', '', line[3:].strip())
            self.blocks.append('if'); self._out(f'if ({self.expr(cond)}) {{'); self.indent += 1; return
        if line.startswith('loop '):
            cond = re.sub(r'(\s+do:?|:)$', '', line[5:].strip())
            self.blocks.append('while'); self._out(f'while ({self.expr(cond)}) {{'); self.indent += 1; return
        if line == 'else' or line.startswith('else if '):
            hdr = 'else' if line == 'else' else f'else if ({self.expr(line[8:])})'
            self.indent -= 1; self._out(f'}} {hdr} {{'); self.indent += 1; return
        if line == 'return': self._out('return;'); return
        if line.startswith('return '): self._out(f'return {self.expr(line[7:])};'); return
        lst = self._decl_all(line)
        if lst and lst[0][0] in TYPES:
            for t, n, v in lst:
                self.scope[n] = t; init = f'= {self.expr(v)}' if v else f'= {self.default_val(t)}'
                self._out(f'{self.c_type(t)} {n} {init};')
            return
        i = find_assign(line)
        if i >= 0:
            lhs, rhs = line[:i].strip(), line[i+1:].strip()
            if not rhs.startswith('new '):
                self._out(f'{self._fields(lhs)} = {self.expr(rhs)};')
            return
        m = _CALL_RE.match(line)
        if m:
            name, rest = m.group(1), m.group(2) or ''; args = split_top(rest, ',') if rest else []
            fn = f'ds_fn_{name}' if name in self.functions else (name if name in BUILTINS else None)
            if fn: self._out(f'{fn}({", ".join(self.expr(a) for a in args)});')

    def generate(self):
        self._infer_returns(); self.output = ['#include "runtime.h"', '#include "net.h"', '#include <math.h>', '']
        for name in self.objects: self._emit(f'typedef struct {name} {name};')
        for name, fields in self.objects.items():
            self._emit(f'struct {name} {{')
            for f, (t, _) in fields.items(): self._emit(f'    {self.c_type(t)} {f};')
            self._emit('};\n' + f'static {name} *ds_new_{name}(void);\nstatic void ds_free_{name}({name} *self);')
        init_lines = []
        for n, (t, v) in self.vars.items():
            if t in self.objects: self._emit(f'{t} *{n} = NULL;'); init_lines.append(n)
            elif v and (_NUM_RE.match(v) or (v.startswith('"') and v.endswith('"'))): self._emit(f'{self.c_type(t)} {n} = {self.expr(v)};')
            else: self._emit(f'{self.c_type(t)} {n} = {self.default_val(t)};')
            if v: init_lines.append(n)
        for n, (params, _) in self.functions.items():
            ret = self.c_type(self.func_ret[n]) if n in self.func_ret else 'void'
            pstr = ', '.join(f'{self.c_type(t)} {pn}' for t, pn in params) if params else 'void'
            self._emit(f'static {ret} ds_fn_{n}({pstr});')
        for name, fields in self.objects.items():
            self._emit(f'static {name} *ds_new_{name}(void) {{\n    {name} *self = ({name} *)calloc(1, sizeof(*self));\n    if (!self) {{ ds_runtime_error("out of memory: {name}"); return NULL; }}')
            for f, (_, v) in fields.items():
                if v: self._emit(f'    self->{f} = {self.expr(v)};')
            self._emit(f'    return self;\n}}\nstatic void ds_free_{name}({name} *self) {{ free(self); }}\n')
        for n, (params, body) in self.functions.items():
            ret = self.c_type(self.func_ret[n]) if n in self.func_ret else 'void'
            pstr = ', '.join(f'{self.c_type(t)} {pn}' for t, pn in params) if params else 'void'
            self._emit(f'static {ret} ds_fn_{n}({pstr}) {{')
            self.indent = 1; self.scope = {pn: pt for pt, pn in params}; self.blocks = []
            body_text = '\n'.join(body)
            for _, pn in params:
                if not used_outside_strings(body_text, pn): self._out(f'(void){pn};')
            for line in body: self._emit_line(line)
            self._emit('}\n')
        self._emit('static int ds_main(void) {'); self.indent = 1; self.scope = {}; self.blocks = []
        for n in init_lines:
            t = self.vars[n][0]
            if t in self.objects: self._out(f'{n} = ds_new_{t}();')
            elif n in self.vars and self.vars[n][1]:
                try: self._out(f'{n} = {self.expr(self.vars[n][1])};')
                except: pass
        for line in self.top: self._emit_line(line)
        self._emit('    return 0;\n}\n\nvoid reset(void) {'); self.indent = 1
        for n, (t, v) in self.vars.items():
            if t in self.objects: self._out(f'if ({n}) ds_free_{t}({n});\n    {n} = NULL;')
            elif t == 'arr': self._out(f'if ({n}) arr_free({n});\n    {n} = arr_new();')
            elif v and (_NUM_RE.match(v) or (v.startswith('"') and v.endswith('"'))): self._out(f'{n} = {self.expr(v)};')
            else: self._out(f'{n} = {self.default_val(t)};')
        self._emit('}\n\nvoid init(AAssetManager *assets) {\n    ds_set_asset_manager(assets);\n    ds_main();')
        if 'init' in self.functions: self._emit('    ds_fn_init();')
        self._emit('}\n\nvoid update(void) {')
        if 'update' in self.functions: self._emit('    ds_fn_update();')
        self._emit('}\n\nvoid draw(Buffer *buffer) {\n    (void)buffer;')
        if 'draw' in self.functions: self._emit('    ds_fn_draw();')
        self._emit('}\n\nvoid touch(float x, float y, int action, int pointer_id) {')
        if 'touch' in self.functions:
            args = [f'({self.c_type(pt)}){("x","y","action","pointer_id")[i]}' for i, (pt, _) in enumerate(self.functions['touch'][0][:4]) if pt != 'str']
            self._emit(f'    ds_fn_touch({", ".join(args)});')
        else: self._emit('    (void)x; (void)y; (void)action; (void)pointer_id;')
        self._emit('}')

    def compile(self, sources, output):
        if not self._load(sources): return False
        self.parse()
        try: self.generate()
        except Exception as exc: self._error(f"internal {exc}"); return False
        with open(output, 'w', encoding='utf-8') as f: f.write('\n'.join(self.output) + '\n')
        return True

def main():
    output = 'game/game.c'; sources = []; args = sys.argv[1:]; i = 0
    while i < len(args):
        if args[i] in ('-o', '--output') and i + 1 < len(args): output = args[i+1]; i += 2
        else: sources.append(args[i]); i += 1
    if not sources: print("Usage: python ds_compiler.py file.ds [-o output.c]", file=sys.stderr); sys.exit(2)
    ok = DimScriptCompiler().compile(sources, output)
    print(f"{output}: {'OK' if ok else 'FAILED'}")
    sys.exit(0 if ok else 1)

if __name__ == '__main__': main()
