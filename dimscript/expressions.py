"""Expressions: interpolation, logic, fields, calls and methods."""

import re

from .tables import (
    BUILTINS, FUNCTION_MAP, NATIVE_MATH, STD_NAMESPACES, _MATH_NS,
)
from .lexer import _NAME, interp_holes, scan, split_top, sub_unquoted


class ExpressionMixin:
    """Expressions: interpolation, logic, fields, calls and methods."""

    def expr(self, e):
        e = e.strip()
        if e.startswith('$"'):
            return self._interp(e)
        m = re.match(r'^new\s+(' + _NAME + r')\s*\(\)$', e)
        if m:
            return f'ds_new_{m.group(1)}()'
        e = self._logic(e)
        if e == 'true':
            return '1'
        if e == 'false':
            return '0'
        parts = split_top(e, '+')
        if len(parts) > 1 and all(parts) and any(self.expr_type(p) == 'str' for p in parts):
            out = self.as_str(parts[0])
            for p in parts[1:]:
                out = f'ds_concat({out}, {self.as_str(p)})'
            return out
        return self._fields(e)

    def _interp(self, e):
        """$"text {expr} text" becomes a chain of ds_concat(...)."""
        parts = interp_holes(e)
        if not parts:
            return '""'
        chunks = []
        for kind, val in parts:
            if kind == 'lit':
                chunks.append('"' + val.replace('\\', '\\\\').replace('"', '\\"') + '"')
            else:
                chunks.append(self.as_str(val))
        out = chunks[0]
        for c in chunks[1:]:
            out = f'ds_concat({out}, {c})'
        return out

    def _logic(self, e):
        """Word operators: and, or, not become &&, || and !."""
        e = e.strip()
        e = sub_unquoted(e, re.compile(r'\band\b'), '&&')
        e = sub_unquoted(e, re.compile(r'\bor\b'), '||')
        e = sub_unquoted(e, re.compile(r'\bnot\b\s*'), '!')
        return e

    def _chain_two(self, e, holder_re, holder_c, cls):
        """self.a.b and obj.a.b become holder->a->b: two fields, no call at the
        end."""
        def repl(m):
            a, b = m.group(1), m.group(2)
            fld = self.objects.get(cls, {}).get(a)
            if not fld or fld[1] not in self.objects:
                return m.group(0)
            if b not in self.objects[fld[1]]:
                return m.group(0)
            return f'{holder_c}->{a}->{b}'
        return re.sub(holder_re, repl, e)

    def _fields(self, e):
        """self.f and obj.f become C pointer fields; calls come after them."""
        if self.cur_class:
            e = self._chain_two(e, r'\bself\.(' + _NAME + r')\.(' + _NAME + r')(?![.(\w])',
                                'self', self.cur_class)
        for n, t in list(self.scope.items()) + [(n, v[1]) for n, v in self.vars.items()]:
            if t in self.objects:
                e = self._chain_two(e, r'\b' + re.escape(n) + r'\.(' + _NAME + r')\.('
                                    + _NAME + r')(?![.(\w])', n, t)
        if self.cur_class and re.search(r'\bself\.', e):
            e = sub_unquoted(e, re.compile(r'\bself\.(' + _NAME + r')(?![\w.(])'), r'self->\1')
        names = [n for n, (_v, t, _d) in self.vars.items() if t in self.objects] + [
            n for n, t in self.scope.items() if t in self.objects
        ]
        for n in sorted(names, key=len, reverse=True):
            e = sub_unquoted(e, re.compile(r'\b' + re.escape(n) + r'\.(' + _NAME + r')(?![\w.(])'), n + r'->\1')
        return self._calls(e)

    def _resolve_dotted(self, e):
        """'self.win.isOpen(a, b)' -> (base_c, 'Win.isOpen', [args]).

        Handles chains like self.a.b(...) and obj.m(...), where the base may be a
        name, self or the result of a call, which is then compiled recursively.
        """
        m = re.match(r'^(.*?)\.(' + _NAME + r')\s*\((.*)\)$', e, re.S)
        if not m:
            return None, None, None
        base_src, method, args_src = m.group(1).strip(), m.group(2), m.group(3)
        args = split_top(args_src, ',') if args_src.strip() else []
        base_src = self._logic(base_src)
        if base_src == 'self':
            cls = self.cur_class
            base_c = 'self'
        elif re.match(r'^' + _NAME + r'$', base_src):
            cls = self._holder_type(base_src)
            base_c = self._fields(base_src)
        else:
            cls = self.expr_type(base_src)
            base_c = '(' + self.expr(base_src) + ')'
        return base_c, (cls, method), args

    def _calls(self, e):
        """Calls: class methods, static methods, namespaces and functions.

        A lookbehind replaces the swallowing prefix: the character before a call
        (the '(' of the previous call, a comma) used to be eaten by the match and
        the next call was left without the ds_fn_ prefix.
        """
        e = sub_unquoted(e, re.compile(r'\bnew\s+(' + _NAME + r')\s*\(\)'), r'ds_new_\1()')
        _, quoted = scan(e)
        out = []
        pos = 0
        for m in re.finditer(r'(?<![.\w])((?:' + _NAME + r'\.)+' + _NAME + r')\s*\(', e):
            if quoted[m.start()]:
                continue
            start = m.start()
            chain = m.group(1)
            pieces = chain.split('.')
            base, method = pieces[0], pieces[-1]
            open_at = m.end() - 1
            close = self._match_paren(e, open_at)
            args_src = e[open_at + 1:close]
            args = split_top(args_src, ',') if args_src.strip() else []
            repl = self._compile_dotted_call(base, pieces[1:-1], method, args,
                                              e[start:close + 1])
            out.append(e[pos:start])
            out.append(repl)
            pos = close + 1
        out.append(e[pos:])
        e = ''.join(out)
        # Free functions and builtins.
        _, quoted = scan(e)
        out = []
        start = 0
        for m in re.finditer(r'(?<![.\w])(' + _NAME + r')\s*\(', e):
            if quoted[m.start()]:
                continue
            name = m.group(1)
            if name in self.functions:
                repl = 'ds_fn_' + name
            elif name in NATIVE_MATH:
                repl = name
            elif name in FUNCTION_MAP:
                repl = FUNCTION_MAP[name]
            elif name in BUILTINS:
                repl = name
            else:
                continue
            out.append(e[start:m.start()])
            out.append(repl)
            start = m.end(1)
        out.append(e[start:])
        return ''.join(out)

    def _match_paren(self, e, open_at):
        depth = 0
        _, quoted = scan(e)
        for i in range(open_at, len(e)):
            if quoted[i]:
                continue
            if e[i] == '(':
                depth += 1
            elif e[i] == ')':
                depth -= 1
                if depth == 0:
                    return i
        return len(e) - 1

    def _compile_dotted_call(self, base, mid, method, args, whole):
        if base == 'self':
            cls = self.cur_class
            base_c = 'self'
            if not cls:
                self._error(f"'self' outside a class method: {whole}")
                return '0'
        elif not mid and base in self.objects:
            return self._call_static(base, method, args, whole)
        elif not mid and base in self.imports:
            path = self.imports[base]
            if path in STD_NAMESPACES:
                ns = STD_NAMESPACES[path]
                pool = BUILTINS if ns == 'Graphics' else _MATH_NS
                if method not in pool:
                    self._error(f"{base}.{method}: no such built-in function")
                    return '0'
                return (FUNCTION_MAP.get(method, method) + '('
                        + ', '.join(self.expr(a) for a in args) + ')')
            if base in self.objects:
                return self._call_static(base, method, args, whole)
            self._error(f"import {path} does not provide the class '{base}': {whole}")
            return '0'
        elif base in self.scope or base in self.vars:
            cls = self._holder_type(base)
            base_c = self._fields(base)
        else:
            self._error(f"unknown call '{whole}'")
            return '0'
        for f in mid:
            fld = self.objects.get(cls, {}).get(f)
            if not fld:
                self._error(f"class '{cls}' has no field '{f}': {whole}")
                return '0'
            if fld[0] == 'private' and cls != self.cur_class:
                self._error(f"private field '{cls}.{f}' read from outside the class")
                return '0'
            base_c = f'{base_c}->{f}'
            cls = fld[1]
        return self._call_method(cls, method, base_c, args, whole)

    def _call_method(self, cls, method, base_c, args, whole):
        m = self.methods.get(cls, {}).get(method)
        if not m:
            self._error(f"class '{cls}' has no method '{method}': {whole}")
            return '0'
        vis, static, params, _ret, _body = m
        if vis == 'private' and cls != self.cur_class:
            self._error(f"private method '{cls}.{method}' called from outside the class")
            return '0'
        if static:
            self._error(f"'{cls}.{method}' is static, called on an instance: {whole}")
            return '0'
        if len(args) != len(params):
            self._error(f"method '{cls}.{method}' wants {len(params)} argument(s), "
                        f"{len(args)} given: {whole}")
            return '0'
        return (f'ds_mth_{cls}_{method}({base_c}' +
                ('' if not args else ', ' + ', '.join(self.expr(a) for a in args)) + ')')

    def _call_static(self, cls, method, args, whole):
        m = self.methods.get(cls, {}).get(method)
        if not m:
            self._error(f"class '{cls}' has no static method '{method}': {whole}")
            return '0'
        vis, static, params, _ret, _body = m
        if not static:
            self._error(f"'{cls}.{method}' is not static, an instance is needed: {whole}")
            return '0'
        if vis == 'private' and cls != self.cur_class:
            self._error(f"private static '{cls}.{method}' called from outside the class")
            return '0'
        if len(args) != len(params):
            self._error(f"static '{cls}.{method}' wants {len(params)} argument(s), "
                        f"{len(args)} given: {whole}")
            return '0'
        return (f'ds_stc_{cls}_{method}(' +
                ', '.join(self.expr(a) for a in args) + ')')

    def as_str(self, e):
        if self.expr_type(e) == 'str':
            return self.expr(e)
        return f'ds_num_to_string((double)({self.expr(e)}))'
