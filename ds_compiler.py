#!/usr/bin/env python3
"""Compile DimScript source into typed C.

Variables become typed C globals/locals (no hash lookup), string ``+`` is
real ``ds_concat``, ``for`` loops are parsed, and ``object`` declarations
become structs with constructors, methods and destructors.
"""

from collections import OrderedDict
import os
import re
import sys


_INCLUDE_RE = re.compile(r'^(?:#\s*)?include\s*(?:"([^"]+)"|<([^>]+)>)\s*;?\s*$')
_DECL_RE = re.compile(
    r'^(?P<type>[A-Za-z_][A-Za-z0-9_]*\*?)\s+'
    r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)'
    r'(?:\s*=\s*(?P<value>.*))?$')
_FN_RE = re.compile(
    r'^fn\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\((?P<params>.*)\)\s*(?P<brace>\{)?\s*$')
_OBJECT_RE = re.compile(
    r'^object\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:\([^)]*\))?\s*(?P<brace>\{)?\s*$')
_NUMBER_RE = re.compile(r'^(?:[-+]?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?|0[xX][0-9a-fA-F]+)$')


def _scan(line):
    """Per-character (depth, in-string) state for splitting expressions."""
    n = len(line)
    depth = [0]*n; quoted = [False]*n
    quote = None; escaped = False; level = 0
    for i, c in enumerate(line):
        quoted[i] = quote is not None
        if escaped: escaped = False; continue
        if c == '\\' and quote: escaped = True; continue
        if quote:
            if c == quote: quote = None
            continue
        if c in ('"', "'"): quote = c
        elif c in '([{': level += 1
        elif c in ')]}': level = max(0, level - 1)
        depth[i] = level
    return depth, quoted


def _strip_inline_comment(line):
    _, q = _scan(line)
    for i in range(len(line)-1):
        if not q[i] and line[i:i+2] == '//': return line[:i]
    return line


def _brace_counts(line):
    depth, q = _scan(line)
    return (sum(1 for i, c in enumerate(line) if c == '{' and not q[i]),
            sum(1 for i, c in enumerate(line) if c == '}' and not q[i]))


def _is_string_literal(v):
    v = v.strip(); return len(v) >= 2 and v[0] == '"' and v[-1] == '"'


def _strip_semicolon(v):
    v = v.rstrip(); return v[:-1].rstrip() if v.endswith(';') else v


def _split_top_level(text, delimiter):
    parts = []; start = 0; depth, q = _scan(text)
    for i, c in enumerate(text):
        if depth[i] == 0 and not q[i] and c == delimiter:
            parts.append(text[start:i].strip()); start = i+1
    parts.append(text[start:].strip())
    return parts


def _strip_outer_parens(expr):
    expr = expr.strip()
    while expr.startswith('(') and expr.endswith(')'):
        _, q = _scan(expr); depth = 0; ok = True
        for i in range(len(expr)):
            if q[i]: continue
            if expr[i] == '(': depth += 1
            elif expr[i] == ')':
                depth -= 1
                if depth == 0 and i != len(expr)-1: ok = False; break
        if not ok or depth: break
        expr = expr[1:-1].strip()
    return expr


def _whole_call(expr):
    expr = expr.strip()
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?)\s*\(', expr)
    if not m: return None
    _, q = _scan(expr)
    oa = expr.find('(', m.start()); depth = 0
    for i in range(oa, len(expr)):
        if q[i]: continue
        if expr[i] == '(': depth += 1
        elif expr[i] == ')':
            depth -= 1
            if depth == 0:
                return (m.group(1), expr[oa+1:i]) if i == len(expr)-1 else None
    return None


def _text_writes_identifier(text, identifier):
    """True if ``identifier`` is assigned anywhere outside string literals."""
    pat = re.compile(r'(?<![.>])\b' + re.escape(identifier) + r'\b\s*(?:\+=|-=|\*=|/=|=)(?!\s*=)')
    _, q = _scan(text)
    return any(not q[m.start()] for m in pat.finditer(text))


def _text_uses_identifier(text, identifier):
    pat = re.compile(r'\b' + re.escape(identifier) + r'\b')
    _, q = _scan(text)
    return any(not q[m.start()] for m in pat.finditer(text))


def _rewrite_outside_strings(text, pattern, repl):
    """Apply a substitution only outside string literals."""
    _, q = _scan(text)
    if not any(q): return re.sub(pattern, repl, text)
    out = []; start = 0; pat = re.compile(pattern)
    for m in pat.finditer(text):
        if q[m.start()]: continue
        out.append(text[start:m.start()])
        out.append(m.expand(repl) if isinstance(repl, str) else repl(m))
        start = m.end()
    out.append(text[start:])
    return ''.join(out)


class DimScriptCompiler:
    BUILTIN_TYPES = {
        'num': 'double', 'int': 'int', 'bool': 'int', 'str': 'const char *',
        'col': 'uint32_t',
    }
    STRING_FUNCTIONS = {'ds_concat', 'ds_num_to_string', 'ds_bool_to_string'}
    NUMBER_FUNCTIONS = {
        'floor', 'ceil', 'round', 'sqrt', 'sin', 'cos', 'tan', 'atan2',
        'fabs', 'abs', 'rand', 'text_ink_width', 'text_ink_height', 'ds_len',
    }

    def __init__(self):
        self.vars = OrderedDict()
        self.functions = OrderedDict()
        self.objects = OrderedDict()
        self.output = []
        self.src = []
        self.errors = 0
        self.loaded_sources = []
        self._loaded_paths = set()
        self._include_stack = []
        self.global_initializers = []
        self.current_scope = {}
        self.current_object = None
        self.current_function = None
        self.object_vars = {}

    def _error(self, msg):
        self.errors += 1
        print(f"DimScript error: {msg}", file=sys.stderr)

    def _load_source(self, path, included_from=None, include_line=0):
        canonical = os.path.realpath(os.path.abspath(os.fspath(path)))
        if canonical in self._include_stack:
            cycle = self._include_stack[self._include_stack.index(canonical):] + [canonical]
            self._error("cyclic include: " + " -> ".join(os.path.basename(p) for p in cycle))
            return False
        if canonical in self._loaded_paths: return True
        if len(self._include_stack) >= 128:
            self._error(f"include nesting is too deep near '{canonical}'"); return False
        if not os.path.isfile(canonical):
            where = f"{included_from}:{include_line}: " if included_from else ""
            self._error(f"{where}source file not found: {path}"); return False
        self._include_stack.append(canonical); self.loaded_sources.append(canonical)
        success = True
        try:
            with open(canonical, 'r', encoding='utf-8-sig') as src:
                for ln, raw in enumerate(src, 1):
                    line = _strip_inline_comment(raw).strip()
                    if not line: continue
                    inc = _INCLUDE_RE.fullmatch(line)
                    if inc:
                        ip = os.path.join(os.path.dirname(canonical), inc.group(1) or inc.group(2))
                        if not self._load_source(ip, canonical, ln): success = False
                        continue
                    if line.startswith('include'):
                        self._error(f"{canonical}:{ln}: malformed include; use #include \"file.ds\"")
                        success = False; continue
                    self.src.append(line)
        except (OSError, UnicodeError) as e:
            self._error(f"cannot read '{canonical}': {e}"); success = False
        finally:
            self._include_stack.pop()
        if success: self._loaded_paths.add(canonical)
        return success

    def parse(self, paths):
        for p in paths:
            if not self._load_source(p): return False
        i = 0
        while i < len(self.src):
            line = self.src[i]
            if _OBJECT_RE.match(line): i = self.parse_object(i)
            elif _FN_RE.match(line): i = self.parse_func(i)
            elif self._looks_like_declaration(line):
                self.parse_var(line); i += 1
            else: i += 1
        self.object_vars = {n: t.rstrip('*') for n, (t, _) in self.vars.items() if t.rstrip('*') in self.objects}
        return self.errors == 0

    def _looks_like_declaration(self, line):
        m = _DECL_RE.match(_strip_semicolon(line.strip()))
        if not m: return False
        t = m.group('type')
        return t in self.BUILTIN_TYPES or t.rstrip('*') in self.objects

    @staticmethod
    def _parse_params(text):
        params = []
        for p in _split_top_level(text, ','):
            p = p.strip()
            if not p: continue
            w = p.split()
            params.append(('num', w[0]) if len(w) == 1 else (w[0], w[-1]))
        return params

    def parse_var(self, line, target=None):
        m = _DECL_RE.match(_strip_semicolon(line.strip()))
        if not m: self._error(f"invalid variable declaration: {line}"); return None
        t, n, v = m.group('type'), m.group('name'), m.group('value')
        target = self.vars if target is None else target
        if n in target: self._error(f"duplicate variable '{n}'")
        else: target[n] = (t, v.strip() if v is not None else None)
        return n, t, v.strip() if v is not None else None

    def _extract_block(self, lines, start, header):
        op, cl = _brace_counts(header); depth = op - cl
        i = start + 1; body = []
        if depth <= 0:
            if i < len(lines) and lines[i].strip() == '{': depth = 1; i += 1
            else: return body, i
        while i < len(lines) and depth > 0:
            cur = lines[i]; o, c = _brace_counts(cur)
            if depth == 1 and cur.strip() == '}' and o == 0 and c == 1: i += 1; break
            body.append(cur); depth += o - c; i += 1
        return body, i

    def parse_func(self, i):
        m = _FN_RE.match(self.src[i])
        if not m: self._error(f"invalid function declaration: {self.src[i]}"); return i+1
        name = m.group('name'); params = self._parse_params(m.group('params'))
        body, ni = self._extract_block(self.src, i, self.src[i])
        depth = sum(a-b for a, b in map(_brace_counts, self.src[i:ni]))
        if depth: self._error(f"function '{name}' has no closing '}}'")
        elif name in self.functions: self._error(f"duplicate function '{name}'")
        else: self.functions[name] = (params, body)
        return ni

    def parse_object(self, i):
        m = _OBJECT_RE.match(self.src[i])
        if not m: self._error(f"invalid object declaration: {self.src[i]}"); return i+1
        name = m.group('name'); lines, ni = self._extract_block(self.src, i, self.src[i])
        fields, methods = OrderedDict(), OrderedDict(); j = 0
        while j < len(lines):
            line = lines[j]; fm = _FN_RE.match(line)
            if fm:
                mn = fm.group('name'); params = self._parse_params(fm.group('params'))
                body, after = self._extract_block(lines, j, line)
                if mn in methods: self._error(f"duplicate method '{name}.{mn}'")
                else: methods[mn] = (params, body)
                j = after; continue
            if self._looks_like_declaration(line):
                p = self.parse_var(line, fields)
                if p and p[1].rstrip('*') in self.objects:
                    self._error(f"nested object fields are not supported yet: {name}.{p[0]}")
                j += 1; continue
            if line not in ('{', '}'): self._error(f"unknown line in object '{name}': {line}")
            j += 1
        if name in self.objects: self._error(f"duplicate object '{name}'")
        else: self.objects[name] = {'fields': fields, 'methods': methods}
        return ni

    def _symbol_type(self, name):
        name = name.strip()
        if name in self.current_scope: return self.current_scope[name]
        if name in self.vars: return self.vars[name][0]
        if name in self.object_vars: return self.object_vars[name]
        m = re.match(r'^([A-Za-z_]\w*)\s*(?:\.|->)\s*([A-Za-z_]\w*)$', name)
        if m:
            o, f = m.groups()
            ot = self.current_object if (o == 'self' and self.current_object) else (
                self.current_scope.get(o) or self.object_vars.get(o))
            if ot in self.objects:
                fi = self.objects[ot]['fields'].get(f)
                if fi: return fi[0]
        if name in ('true', 'false'): return 'bool'
        if name.startswith('"'): return 'str'
        if name in self.STRING_FUNCTIONS: return 'str'
        return None

    def infer_expr_type(self, expr):
        expr = _strip_outer_parens(expr.strip())
        if not expr: return None
        if _is_string_literal(expr): return 'str'
        parts = _split_top_level(expr, '+')
        if len(parts) > 1 and all(parts):
            return 'str' if any(self.infer_expr_type(p) == 'str' for p in parts) else 'num'
        if re.search(r'(^|\s)(==|!=|<=|>=|<|>)(\s|$)', expr): return 'bool'
        call = _whole_call(expr)
        if call:
            bare = call[0].split('.')[-1]
            if bare in self.STRING_FUNCTIONS: return 'str'
            if bare in self.NUMBER_FUNCTIONS or bare in self.functions: return 'num'
        s = self._symbol_type(expr)
        return s or 'num'

    def _translate_object_access(self, expr):
        expr = _rewrite_outside_strings(expr, r'\btrue\b', '1')
        expr = _rewrite_outside_strings(expr, r'\bfalse\b', '0')
        ko = dict(self.object_vars)
        for v, st in self.current_scope.items():
            if st.rstrip('*') in self.objects: ko[v] = st.rstrip('*')
        for v, ot in sorted(ko.items(), key=lambda it: -len(it[0])):
            expr = _rewrite_outside_strings(
                expr, rf'\b{re.escape(v)}\.([A-Za-z_]\w*)', rf'{v}->\1')
        if self.current_object:
            expr = _rewrite_outside_strings(expr, r'\bself\.([A-Za-z_]\w*)', r'self->\1')
        return expr

    def _as_string_expression(self, expr):
        expr = self.translate_expression(expr); t = self.infer_expr_type(expr)
        if t == 'str': return expr
        if t == 'bool': return f'ds_bool_to_string((int)({expr}))'
        return f'ds_num_to_string((double)({expr}))'

    def translate_expression(self, expr):
        expr = expr.strip()
        if not expr: return expr
        stripped = _strip_outer_parens(expr)
        parts = _split_top_level(stripped, '+')
        if len(parts) > 1 and all(parts):
            types = [self.infer_expr_type(p) for p in parts]
            if any(t == 'str' for t in types):
                out = self._as_string_expression(parts[0])
                for p in parts[1:]: out = f'ds_concat({out}, {self._as_string_expression(p)})'
                return out
            return self._translate_object_access(expr)
        call = _whole_call(stripped)
        if call:
            name, args = call
            return self._translate_object_access(
                f'{name}({", ".join(self.translate_expression(a) for a in _split_top_level(args, ","))})')
        return self._translate_object_access(expr)

    def c_type(self, st):
        if st in self.BUILTIN_TYPES: return self.BUILTIN_TYPES[st]
        ot = st.rstrip('*')
        return f'{ot} *' if ot in self.objects else st

    def _default_c_value(self, st, value=None):
        if value and value.strip(): return self.translate_expression(value)
        if st in ('str',) or st.endswith('*') or st.rstrip('*') in self.objects: return 'NULL'
        return '0'

    def _is_c_static_expression(self, v):
        v = v.strip()
        return _is_string_literal(v) or bool(_NUMBER_RE.match(v)) or v in ('NULL', 'true', 'false')

    @staticmethod
    def _declaration(c_type, name, prefix=''):
        if prefix: c_type = f'{prefix} {c_type}'
        return f'{c_type}{name}' if c_type.endswith('*') else f'{c_type} {name}'

    @staticmethod
    def _param_c(c_type, name, is_const):
        if is_const and '*' not in c_type and 'const' not in c_type:
            return DimScriptCompiler._declaration(c_type, name, 'const')
        return DimScriptCompiler._declaration(c_type, name)

    def _params_c(self, params, const_names=None):
        if not params: return 'void'
        const_names = const_names or set()
        return ', '.join(self._param_c(self.c_type(t), n, n in const_names) for t, n in params)

    def _object_signature(self, on, mn, params, const_names=None):
        const_names = const_names or set()
        ps = [f'{on} *self'] + [self._param_c(self.c_type(t), n, n in const_names) for t, n in params]
        return f'static void ds_obj_{on}_{mn}({", ".join(ps)})'

    def _const_param_names(self, params, body):
        body_text = '\n'.join(body)
        return {n for pt, n in params
                if '*' not in self.c_type(pt) and 'const' not in self.c_type(pt)
                and not _text_writes_identifier(body_text, n)}

    def _emit_body(self, body, params, object_name, function_name):
        ps, po, pf = self.current_scope, self.current_object, self.current_function
        self.current_scope = dict(params); self.current_object = object_name; self.current_function = function_name
        # (void) только для неиспользуемых параметров.
        bt = '\n'.join(body)
        for p in params:
            if not _text_uses_identifier(bt, p): self._indent(f'(void){p};')
        for line in body: self.compile_line(line)
        self.current_scope, self.current_object, self.current_function = ps, po, pf

    def _indent(self, line): self.output.append(f'    {line}')
    def emit(self, line): self.output.append(line)

    def generate(self):
        self.output = ['#include "runtime.h"', '#include <math.h>', '']
        fc = {n: self._const_param_names(p, b) for n, (p, b) in self.functions.items()}
        mc = {on: {m: self._const_param_names(p, b)
                   for m, (p, b) in d['methods'].items()}
              for on, d in self.objects.items()}

        for n in self.objects: self.emit(f'typedef struct {n} {n};')
        if self.objects: self.emit('')
        for n, d in self.objects.items():
            self.emit(f'struct {n} {{')
            if not d['fields']: self.emit('    unsigned char _ds_empty;')
            for f, (ft, _) in d['fields'].items(): self.emit(f'    {self.c_type(ft)} {f};')
            self.emit('};')
        if self.objects: self.emit('')

        for n, d in self.objects.items():
            for m, (p, _) in d['methods'].items():
                self.emit(f'{self._object_signature(n, m, p, mc[n].get(m))};')
            self.emit(f'static {n} *ds_new_{n}(' +
                      self._params_c(self._ctor_params(d), mc[n].get("init")) + ');')
            self.emit(f'static void ds_free_{n}({n} *self);')
        if self.objects: self.emit('')

        for n, (st, v) in self.vars.items():
            c_type = self.c_type(st)
            if st.rstrip('*') in self.objects:
                initial = 'NULL'
                if v: self.global_initializers.append((n, v))
            else:
                sconv = st == 'str' and v and v.strip() and self.infer_expr_type(v) != 'str'
                if v and v.strip() and self._is_c_static_expression(v) and not sconv:
                    initial = self.translate_expression(v)
                else:
                    initial = self._default_c_value(st)
                    if v: self.global_initializers.append((n, v))
            self.emit(f'{self._declaration(c_type, n)} = {initial};')
        if self.vars: self.emit('')

        for n, (p, _) in self.functions.items():
            self.emit(f'static void ds_fn_{n}({self._params_c(p, fc[n])});')
        if self.functions: self.emit('')

        for n, d in self.objects.items():
            for m, (p, b) in d['methods'].items():
                self.emit(self._object_signature(n, m, p, mc[n].get(m)) + ' {')
                ms = {'self': n}; ms.update({pp: t for t, pp in p})
                self._emit_body(b, ms, n, m); self.emit('}'); self.emit('')
            self.emit(f'static {n} *ds_new_{n}(' + self._params_c(self._ctor_params(d), mc[n].get('init')) + ') {')
            self.emit(f'    {n} *self = ({n} *)calloc(1, sizeof(*self));')
            self.emit(f'    if (!self) {{ ds_runtime_error("out of memory creating object {n}"); return NULL; }}')
            for f, (ft, v) in d['fields'].items():
                if v and v.strip():
                    fv = (self._as_string_expression(v)
                          if ft == 'str' and self.infer_expr_type(v) != 'str'
                          else self.translate_expression(v))
                    self.emit(f'    self->{f} = {fv};')
            if 'init' in d['methods']:
                args = ', '.join(p for _t, p in self._ctor_params(d))
                self.emit(f'    ds_obj_{n}_init(self' + (f', {args}' if args else '') + ');')
            self.emit('    return self;\n}\n')
            self.emit(f'static void ds_free_{n}({n} *self) {{\n    free(self);\n}}\n')

        for n, (p, b) in self.functions.items():
            self.emit(f'static void ds_fn_{n}({self._params_c(p, fc[n])}) {{')
            self._emit_body(b, {pp: t for t, pp in p}, None, n); self.emit('}\n')

        # ds_main нужен только для неконстантных инициализаторов глобалов
        # (например, object-переменных, создаваемых через new).
        has_main = bool(self.global_initializers)
        if has_main:
            self.emit('static int ds_main(void) {')
            for n, v in self.global_initializers: self.compile_line(f'{n} = {v}')
            self.emit('    return 0;\n}\n')

        self.emit('void reset(void) {')
        for n, (st, v) in self.vars.items():
            ot = st.rstrip('*')
            if ot in self.objects:
                self.emit(f'    if ({n}) ds_free_{ot}({n});\n    {n} = NULL;')
            elif v and self._is_c_static_expression(v):
                self.emit(f'    {n} = {self.translate_expression(v)};')
            else:
                self.emit(f'    {n} = {self._default_c_value(st)};')
        self.emit('}\n')
        self.emit('void init(AAssetManager *assets) {')
        self.emit('    ds_set_asset_manager(assets);')
        if has_main: self.emit('    ds_main();')
        if 'init' in self.functions: self.emit('    ds_fn_init();')
        self.emit('}\n')
        self.emit('void update(void) {')
        if 'update' in self.functions: self.emit('    ds_fn_update();')
        if 'update_touch' in self.functions: self.emit('    ds_fn_update_touch();')
        self.emit('}\n')
        self.emit('void draw(Buffer *buffer) {\n    (void)buffer;')
        if 'draw' in self.functions: self.emit('    ds_fn_draw();')
        self.emit('}\n')
        self.emit('void touch(float x, float y, int action) {')
        if 'touch' in self.functions:
            # Приводим runtime-значения к типам параметров, чтобы int не шёл через double.
            params = self.functions['touch'][0]
            rv = ('x', 'y', 'action')
            args = ', '.join(f'({self.c_type(pt)}){rv[i]}' for i, (pt, _) in enumerate(params[:len(rv)]))
            self.emit(f'    ds_fn_touch({args});')
        else:
            self.emit('    (void)x; (void)y; (void)action;')
        self.emit('}')

    @staticmethod
    def _ctor_params(definition):
        """Constructor parameters match the user-defined init method, if any."""
        init = definition['methods'].get('init')
        return init[0] if init else []

    def compile_line(self, raw):
        line = _strip_semicolon(raw.strip())
        if not line: return
        decl = _DECL_RE.match(line)
        if decl and (decl.group('type') in self.BUILTIN_TYPES or decl.group('type').rstrip('*') in self.objects):
            self._compile_declaration(decl, line); return
        if line.startswith('for'):
            m = re.match(r'^for\s*\((.*)\)\s*\{?$', line)
            if m:
                parts = _split_top_level(m.group(1), ';')
                if len(parts) == 3:
                    init, cond, step = (p.strip() for p in parts)
                    self._indent(f'for ({self._translate_for_init(init)}; '
                                 f'{self.translate_expression(cond)}; '
                                 f'{self.translate_expression(step)}) {{')
                    return
            self._error(f"invalid for loop: {raw}"); return
        for kw, off in (('if', 2), ('while', 5), ('loop', 5)):
            if line.startswith(kw + ' '):
                ck = 'while' if kw == 'loop' else kw
                # Однострочная форма if/while (cond) stmt — без фигурных скобок.
                if not line.endswith('{'):
                    rest = line[off:].strip()
                    # cond — это balanced-выражение в скобках
                    if not rest.startswith('('):
                        self._error(f"invalid {kw} syntax: {raw}"); return
                    depth, q = _scan(rest)
                    end = 0
                    for i in range(len(rest)):
                        if q[i]: continue
                        if rest[i] == '(': depth[i] += 1
                        elif rest[i] == ')': depth[i] -= 1
                        if depth[i] == 0 and i > 0:
                            end = i; break
                    cond = rest[:end+1]
                    body = rest[end+1:].strip()
                    body_c = self.translate_expression(body) if body else '(void)0'
                    self._indent(f'{ck} ({self.translate_expression(cond)}) {{ {body_c}; }}'); return
                # Многострочная форма cond { ... } — cond до {
                cond = line[off:].strip()
                if cond.endswith('{'): cond = cond[:-1].strip()
                self._indent(f'{ck} ({self.translate_expression(_strip_outer_parens(cond))}) {{'); return
        if line == 'else' or line == 'else {': self._indent('else {'); return
        if line.startswith('else if '):
            cond = line[8:].strip()
            if cond.endswith('{'): cond = cond[:-1].strip()
            self._indent(f'else if ({self.translate_expression(_strip_outer_parens(cond))}) {{'); return
        if line.startswith('return') and (line == 'return' or line[6:7] in (' ', ';')):
            v = line[6:].strip()
            self._indent(f'return {self.translate_expression(v)};' if v else 'return;'); return
        new = re.match(r'^([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_]\w*)\s*(?:\((.*)\))?$', line)
        if new and new.group(2) in self.objects:
            var, ot = new.group(1), new.group(2)
            self._indent(f'{var} = ds_new_{ot}({self._translate_args(new.group(3) or "")});')
            self.current_scope[var] = ot; return
        if line.startswith('delete '):
            v = line[7:].strip()
            ot = self.current_scope.get(v) or self.object_vars.get(v)
            if ot in self.objects:
                self._indent(f'ds_free_{ot}({v}); {v} = NULL;')
            else: self._indent(f'free({v}); {v} = NULL;')
            return
        if line.startswith('}'):
            self._indent('}')
            rest = line[1:].strip()
            if rest: self.compile_line(rest)
            return
        if line.endswith('{'): self._indent(line); return
        call = _whole_call(line)
        if call: self._compile_call(call[0], call[1]); return
        if self._find_assignment(line): self.compile_assign(line); return
        self._indent(self.translate_expression(line) + ';')

    def _compile_declaration(self, decl, line):
        st, n, v = decl.group('type'), decl.group('name'), decl.group('value')
        ot = st.rstrip('*')
        if ot in self.objects:
            self.current_scope[n] = ot
            nm = re.match(r'new\s+([A-Za-z_]\w*)\s*(?:\((.*)\))?$', v.strip()) if v and v.strip().startswith('new ') else None
            if nm and nm.group(1) == ot:
                self._indent(f'{ot} *{n} = ds_new_{ot}({self._translate_args(nm.group(2) or "")});')
            else: self._indent(f'{ot} *{n} = NULL;')
            return
        self.current_scope[n] = st
        c = self._declaration(self.c_type(st), n)
        if v and v.strip():
            c += ' = ' + (self._as_string_expression(v)
                          if st == 'str' and self.infer_expr_type(v) != 'str'
                          else self.translate_expression(v))
        self._indent(c + ';')

    def _translate_for_init(self, init):
        decl = _DECL_RE.match(init)
        if decl and decl.group('type') in self.BUILTIN_TYPES:
            t, n, v = decl.group('type'), decl.group('name'), decl.group('value')
            self.current_scope[n] = t
            r = self._declaration(self.c_type(t), n)
            if v: r += f' = {self.translate_expression(v)}'
            return r
        return self.translate_expression(init)

    def _translate_args(self, args):
        if not args.strip(): return ''
        return ', '.join(self.translate_expression(a) for a in _split_top_level(args, ','))

    def _compile_call(self, name, args_text):
        args = self._translate_args(args_text)
        bare = name.split('.')[-1]
        if '.' in name:
            on, m = name.split('.', 1)
            ot = self.current_scope.get(on) or self.object_vars.get(on)
            if ot in self.objects and m in self.objects[ot]['methods']:
                call = f'ds_obj_{ot}_{m}({on}'
                if args: call += f', {args}'
                self._indent(call + ');'); return
        if bare in self.functions: self._indent(f'ds_fn_{bare}({args});')
        else: self._indent(f'{self._translate_object_access(name)}({args});')

    @staticmethod
    def _find_assignment(line):
        depth, q = _scan(line)
        for i, c in enumerate(line):
            if q[i] or depth[i]: continue
            if c == '=' and (i == 0 or line[i-1] not in '<>!=') and (i+1 >= len(line) or line[i+1] != '='):
                return True
            if c in '+-*/' and i+1 < len(line) and line[i+1] == '=': return True
        return False

    @staticmethod
    def _assignment_parts(line):
        m = re.match(r'^(.+?)\s*(\+=|-=|\*=|/=|=)\s*(.*)$', line)
        return m.groups() if m else None

    def compile_assign(self, line):
        parts = self._assignment_parts(line)
        if not parts: self._indent(self.translate_expression(line) + ';'); return
        left, op, right = (p.strip() for p in parts)
        lt, rt = self.infer_expr_type(left), self.infer_expr_type(right)
        left = self._translate_object_access(left)
        if op == '+=' and (lt == 'str' or rt == 'str'):
            self._indent(f'{left} = ds_concat({left}, {self._as_string_expression(right)});'); return
        if op == '=' and lt == 'str' and rt != 'str':
            self._indent(f'{left} = {self._as_string_expression(right)};'); return
        self._indent(f'{left} {op} {self.translate_expression(right)};')

    def compile(self, sources, output):
        if not self.parse(sources): return False
        self.generate()
        if self.errors: return False
        with open(output, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.output) + '\n')
        return True


def main():
    if len(sys.argv) < 2:
        print("Usage: python ds_compiler.py file.ds -o output.c"); sys.exit(2)
    output = 'game/game.c'; sources = []; i = 1
    while i < len(sys.argv):
        if sys.argv[i] in ('-o', '--output'):
            output = sys.argv[i+1] if i+1 < len(sys.argv) else 'game/game.c'; i += 2
        else: sources.append(sys.argv[i]); i += 1
    if not sources: print("Error: no input files"); sys.exit(2)
    sys.exit(0 if DimScriptCompiler().compile(sources, output) else 1)


if __name__ == '__main__':
    main()
