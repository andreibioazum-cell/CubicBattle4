"""Types and strictness: C types, expression types, name and type checks."""

import re

from .tables import (
    BUILTINS, ENGINE_VARS, NATIVE_MATH, STR_BUILTINS, TYPES, canon_type,
)
from .lexer import _NAME, _NUM_RE, interp_holes, scan


class TypesMixin:
    """Types and strictness: C types, expression types, name and type checks."""

    def c_type(self, t):
        if t in TYPES:
            return TYPES[t]
        if t in self.objects:
            return t + ' *'
        return 'double'

    def default_value(self, t):
        return 'NULL' if t == 'str' else '0'

    def static_expr(self, v):
        return bool(_NUM_RE.match(v)) or (len(v) >= 2 and v[0] == '"' and v[-1] == '"')

    def _holder_type(self, name):
        if name == 'self':
            return self.cur_class
        if name in self.scope:
            return self.scope[name]
        if name in self.vars:
            return self.vars[name][1]
        return None

    def expr_type(self, expr):
        expr = expr.strip()
        if expr.startswith('$"'):
            return 'str'
        if expr.startswith('"') and expr.endswith('"'):
            return 'str'
        if expr.startswith('new '):
            m = re.match(r'^new\s+(' + _NAME + r')\s*\(\)$', expr)
            return m.group(1) if m else 'num'
        m = re.match(r'^(' + _NAME + r')\.(' + _NAME + r')$', expr)
        if m:
            ot = self._holder_type(m.group(1))
            fields = self.objects.get(ot)
            if fields and m.group(2) in fields:
                return fields[m.group(2)][1]
            return None
        if expr == 'self':
            return self.cur_class
        if expr in self.scope:
            return self.scope[expr]
        if expr in self.vars:
            return self.vars[expr][1]
        if expr in ENGINE_VARS:
            return ENGINE_VARS[expr]
        call = re.match(r'^(' + _NAME + r')\s*\(.*\)$', expr)
        if call:
            name = call.group(1)
            if name in self.func_ret:
                r = self.func_ret[name]
                return None if r == 'void' else r
            if name in STR_BUILTINS:
                return 'str'
            if name in BUILTINS or name in NATIVE_MATH:
                return 'num'
        mcall = re.match(r'^(self|' + _NAME + r')(?:\.(' + _NAME + r'))+\s*\(.*\)$', expr)
        if mcall:
            return self._method_ret_type(expr)
        return None

    def _method_ret_type(self, expr):
        m = re.match(r'^(self|' + _NAME + r')((?:\.' + _NAME + r')+)?\s*\(', expr)
        if not m:
            return None
        pieces = [m.group(1)] + [p for p in (m.group(2) or '').split('.') if p]
        base, method = pieces[0], pieces[-1]
        mids = pieces[1:-1]
        if base == 'self':
            cls = self.cur_class
        elif base in self.imports:
            return None
        elif not mids and base in self.objects:
            cls, method = base, pieces[-1]
            mth = self.methods.get(cls, {}).get(method)
            if not mth or not mth[1]:
                return None
            r = self.func_ret.get(f'{cls}.{method}')
            return None if r == 'void' else r
        elif base in self.scope or base in self.vars:
            cls = self._holder_type(base)
        else:
            return None
        for f in mids:
            fld = self.objects.get(cls, {}).get(f)
            if not fld:
                return None
            cls = fld[1]
        mth = self.methods.get(cls, {}).get(method)
        if not mth or mth[1]:
            return None
        r = self.func_ret.get(f'{cls}.{method}')
        return None if r == 'void' else r

    def _check_idents(self, e, where):
        """No undeclared name may appear in an expression, per v2 strictness."""
        for m in re.finditer(r'\$"(?:[^"\\]|\\.)*"', e):
            for kind, val in interp_holes(m.group(0)):
                if kind == 'hole':
                    self._check_idents(val, where)
        e = re.sub(r'\$"(?:[^"\\]|\\.)*"', '""', e)
        depth, quoted = scan(e)
        for m in re.finditer(r'\b' + _NAME + r'\b', e):
            i = m.start()
            if quoted[i] or depth[i]:
                continue
            if i > 0 and e[i - 1] == '.':
                continue  # a field or method is checked separately
            nxt = e[m.end():m.end() + 1]
            name = m.group(0)
            if nxt == '(':
                if (name in self.functions or name in BUILTINS or name == 'new'
                        or name in self.objects or name in self.imports
                        or name in NATIVE_MATH):
                    continue
                self._error(f"{where}: unknown call '{name}(...)'")
                continue
            if name in ('true', 'false', 'and', 'or', 'not', 'new', 'self',
                        'to', 'step', 'do', 'then'):
                continue
            if name in NATIVE_MATH or name in ('unsigned', 'int', 'double',
                                                'char', 'float'):
                continue  # real C casts and math.h inside expressions
            if name in self.scope or name in self.vars or name in ENGINE_VARS:
                continue
            if name in self.objects or name in self.imports:
                continue  # the base of a chain or a class name
            self._error(f"{where}: unknown name '{name}'")

    def _check_types(self, lhs_type, rhs, where):
        rt = self.expr_type(rhs)
        if lhs_type is None or rt is None:
            return
        if lhs_type == rt:
            return
        if lhs_type in TYPES and rt in TYPES:
            if {canon_type(lhs_type), canon_type(rt)} == {'num', 'str'}:
                self._error(f"{where}: incompatible types {lhs_type} and {rt}")
            return
        if lhs_type in self.objects or rt in self.objects:
            self._error(f"{where}: incompatible types {lhs_type} and {rt}")
