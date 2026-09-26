"""Statements: blocks, loops, calls, assignments and compound operators."""

import re

from .tables import BUILTINS, ENGINE_VARS, FUNCTION_MAP, TYPES, canon_type
from .lexer import (
    _FOR_RE, _NAME, find_assign, find_compound, num_value, split_top,
)


class EmitterMixin:
    """Statements: blocks, loops, calls, assignments and compound operators."""

    def _out(self, s):
        self.output.append('    ' * self.indent + s)

    def _emit(self, s):
        self.output.append(s)

    def _emit_line(self, line, where):
        if line == 'end':
            if not self.blocks:
                self._error(f"unexpected 'end': {where}")
                return
            _header, footer = self.blocks.pop()
            self.indent -= 1
            if footer:
                self._out(footer)
            self._out('}')
            return
        if line.startswith('if '):
            cond = line[3:].strip()
            if not cond.endswith('then'):
                self._error(f"v2: 'if <condition> then' (without then no block opens): {line}")
                return
            self._open_block(f'if ({self.expr(cond[:-4].strip())})')
            return
        if line.startswith('while '):
            cond = line[6:].strip()
            if not cond.endswith('do'):
                self._error(f"v2: 'while <condition> do': {line}")
                return
            self._open_block(f'while ({self.expr(cond[:-2].strip())})')
            return
        if line.startswith('loop '):
            self._error(f"v2: 'loop' became 'while ... do': {line}")
            return
        if line.startswith('for '):
            self._open_for(line)
            return
        if line in ('break', 'continue'):
            if not any(h.startswith('while (') for h, _f in self.blocks):
                self._error(f"'{line}' outside of a loop: {line}")
                return
            self._out(line + ';')
            return
        if line == 'else' or line.startswith(('else if ', 'elif ')):
            if not self.blocks:
                self._error(f"'else' without 'if': {line}")
                return
            if line == 'else':
                header = 'else'
            else:
                rest = line[8:] if line.startswith('else if ') else line[5:]
                rest = rest.strip()
                if not rest.endswith('then'):
                    self._error(f"v2: 'else if <condition> then': {line}")
                    return
                header = 'else if (' + self.expr(rest[:-4].strip()) + ')'
            self.indent -= 1
            self._out(f'}} {header} {{')
            self.indent += 1
            return
        if line == 'return':
            self._out('return;')
            return
        if line.startswith('return '):
            rhs = line[7:].strip()
            self._check_idents(rhs, where)
            self._out(f'return {self.expr(rhs)};')
            return
        lm = re.match(r'^local\s+(' + _NAME + r')\s*(?:=\s*(.+))?$', line)
        if lm and canon_type(lm.group(1)) not in TYPES and lm.group(1) not in self.objects:
            name, v = lm.group(1), (lm.group(2) or '').strip()
            if name in self.scope:
                self._error(f"dup var '{name}'")
                return
            t = self.expr_type(v) if v else None
            t = t if t in TYPES or t in self.objects else 'num'
            self._check_idents(v, where) if v else None
            self.scope[name] = t
            init = f'= {self.expr(v)}' if v else f'= {self.default_value(t)}'
            self._out(f'{self.c_type(t)} {name} {init};')
            return
        lst = self._decl_list(line)
        if lst and lst[0][1]:  # local ...
            for vis, _loc, t, n, v in lst:
                if vis:
                    self._error(f"v2: local takes no modifier: {line}")
                    continue
                if n in self.scope:
                    self._error(f"dup var '{n}'")
                    continue
                if v:
                    self._check_idents(v, where)
                    self._check_types(t, v, f"local '{n}'")
                    init = f'= {self.expr(v)}'
                else:
                    init = f'= {self.default_value(t)}'
                self.scope[n] = t
                self._out(f'{self.c_type(t)} {n} {init};')
            return
        if lst and not lst[0][1] and not lst[0][0]:
            self._error(f"v2: inside a function locals are declared with local: {line}")
            return
        if lst:
            self._error(f"v2: no public or private modifier inside a function, use local: {line}")
            return
        self._emit_statement(line, where)

    def _open_block(self, header, footer=None):
        self.blocks.append((header, footer))
        self._out(header + ' {')
        self.indent += 1

    def _open_for(self, line):
        m = _FOR_RE.match(line)
        if not m:
            self._error(
                "v2: a loop is 'for <var> = <from> to <to> [step <n>] do': " + line)
            return
        var, lo, hi, step = m.group(1), m.group(2), m.group(3), (m.group(4) or '').strip()
        nums = [num_value(lo), num_value(hi)]
        down = all(v is not None for v in nums) and nums[0] > nums[1]
        op = '>=' if down else '<='
        step_expr = self.expr(step) if step else ('-1' if down else '1')
        if var in self.scope or var in self.vars:
            self._out(f'{self._fields(var)} = {self.expr(lo)};')
        else:
            self.scope[var] = 'num'
            self._out(f'{self.c_type("num")} {var} = {self.expr(lo)};')
        hi_expr = self.expr(hi)
        self._open_block(
            f'while ({var} {op} {hi_expr})',
            footer=f'{self._fields(var)} += {step_expr};')

    def _split_call(self, line):
        m = re.match(r'^(' + _NAME + r')\s*\((.*)\)\s*$', line, re.S)
        if m:
            return m.group(1), (m.group(2) or '').strip()
        return None, None

    def _emit_statement(self, line, where):
        ci = find_compound(line)
        if ci >= 0:
            self._emit_compound(line[:ci].strip(), line[ci], line[ci + 2:].strip(), where)
            return
        i = find_assign(line)
        if i >= 0:
            self._emit_assign(line[:i].strip(), line[i + 1:].strip(), where)
            return
        # A call statement needs brackets in v2.
        if re.match(r'^' + _NAME + r'\s+[^=]+$', line) and not line.startswith(('local ', 'return ')):
            self._error(f"v2: a call needs brackets: '{line.split()[0]}(...)': {line}")
            return
        name, rest = self._split_call(line)
        if not name:
            # A method, self or chain as a statement: self.m(x), obj.m(x), Ns.f(x).
            if re.match(r'^(self|' + _NAME + r')\.', line):
                self._check_idents(line, where)
                self._out(self.expr(line) + ';')
                return
            self._error(f"v2: unrecognised statement: {line}")
            return
        args = split_top(rest, ',') if rest else []
        for a in args:
            self._check_idents(a, where)
        if name in self.functions:
            vis, params, _body = self.functions[name]
            if len(args) != len(params):
                self._error(f"call '{name}' expects {len(params)} arg(s), got {len(args)}: {line}")
                return
            fn = f'ds_fn_{name}'
        elif name in BUILTINS:
            fn = FUNCTION_MAP.get(name, name)
        else:
            self._error(f"unknown call '{name}' (v2: declare the function or add "
                        f"the native one to BUILTINS): {line}")
            return
        self._out(f'{fn}({", ".join(self.expr(a) for a in args)});')

    def _emit_compound(self, lhs, op, rhs, where):
        m = re.match(r'^(self\.)?(' + _NAME + r')(?:\.(' + _NAME + r'))?$', lhs)
        if not m:
            self._error(f"cannot assign to '{lhs}': {lhs} {op}= {rhs}")
            return
        self_ref, name, field = m.group(1), m.group(2), m.group(3)
        if self_ref:
            if field:
                self._error(f"{where}: assignment to the chain self.{name}.{field} is not supported")
                return
            if not self._check_field_write('self', name, where):
                return
            target = f'self->{name}'
        else:
            if name not in self.scope and name not in self.vars and name not in ENGINE_VARS:
                self._error(f"undeclared variable in '{lhs} {op}= {rhs}'")
                return
            target = self._fields(lhs)
        self._check_idents(rhs, where)
        self._out(f'{target} {op}= {self.expr(rhs)};')

    def _check_field_write(self, name, field, where):
        cls = self.cur_class
        if name != 'self' or not cls:
            self._error(f"{where}: assignment to a field without self.: {name}.{field}")
            return False
        f = self.objects.get(cls, {}).get(field)
        if not f:
            self._error(f"{where}: class '{cls}' has no field '{field}'")
            return False
        return True

    def _emit_assign(self, lhs, rhs, where):
        m = re.match(r'^(self\.)?(' + _NAME + r')(?:\.(' + _NAME + r'))?$', lhs)
        if not m:
            self._error(f"v2: unrecognised assignment: {lhs} = {rhs}")
            return
        self_ref, name, field = m.group(1), m.group(2), m.group(3)
        self._check_idents(rhs, where)
        if self_ref:
            if field:
                self._error(f"{where}: assignment to the chain self.{name}.{field} is not supported")
                return
            if not self._check_field_write('self', name, where):
                return
            f = self.objects[self.cur_class][name]
            self._check_types(f[1], rhs, f"field '{self.cur_class}.{name}'")
            self._out(f'self->{name} = {self.expr(rhs)};')
            return
        if rhs.startswith('new '):
            mm = re.match(r'^new\s+(' + _NAME + r')\s*\(\)$', rhs)
            if name in self.scope and mm and self.scope[name] in self.objects:
                self._out(f'{name} = ds_new_{mm.group(1)}();')
                return
            if name in self.vars and mm and self.vars[name][1] in self.objects:
                self._out(f'{name} = ds_new_{mm.group(1)}();')
                return
            self._error(f"v2: 'new' is only allowed in a declaration: {lhs} = {rhs}")
            return
        if name not in self.scope and name not in self.vars and name not in ENGINE_VARS:
            self._error(f"assignment to the undeclared variable '{name}': {lhs} = {rhs}")
            return
        holder_type = self._holder_type(name)
        if field:
            if holder_type not in self.objects:
                if name in ENGINE_VARS:
                    self._out(f'{lhs} = {self.expr(rhs)};')
                    return
                self._error(f"'{name}' is not a class object: {lhs} = {rhs}")
                return
            f = self.objects[holder_type].get(field)
            if not f:
                self._error(f"class '{holder_type}' has no field '{field}'")
                return
            if f[0] == 'private' and holder_type != self.cur_class:
                self._error(f"private field '{holder_type}.{field}' set from outside the class")
                return
            self._check_types(f[1], rhs, f"field '{holder_type}.{field}'")
            self._out(f'{self._fields(lhs)} = {self.expr(rhs)};')
            return
        self._check_types(holder_type, rhs, f"variable '{name}'")
        self._out(f'{name} = {self.expr(rhs)};')
