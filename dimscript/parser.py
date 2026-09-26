"""Parsing: classes, functions, parameters, globals and blocks."""

import re

from .tables import STD_NAMESPACES, TYPES, canon_type
from .lexer import _NAME, split_top


class ParserMixin:
    """Parsing: classes, functions, parameters, globals and blocks."""

    def _decl_list(self, line):
        """[public|private] [local] type a=1, b=2 -> [(vis, local, type, name, val)]."""
        m = re.match(
            r'^(?:(public|private)\s+)?(local\s+)?(' + _NAME + r')\s+(.+)$', line)
        if not m:
            return None
        vis, local, t, rest = m.group(1), bool(m.group(2)), canon_type(m.group(3)), m.group(4).strip()
        if t not in TYPES and t not in self.objects and t != 'joy':
            return None
        res = []
        for part in split_top(rest, ','):
            part = part.strip()
            if not part:
                continue
            mm = re.match(r'^(' + _NAME + r')(?:\s*=\s*(.*))?$', part)
            if not mm:
                return None
            v = mm.group(2)
            res.append((vis, local, t, mm.group(1), v.strip() if v else v))
        return res or None

    def parse(self):
        for line in self.lines:
            m = re.match(r'^(?:(?:public|private)\s+)?class\s+(' + _NAME + r')\s*$', line)
            if m and m.group(1) not in self.objects:
                self.objects[m.group(1)] = {}
        i = 0
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                self._error("unexpected 'end' at top level")
                i += 1
            elif line.startswith('include '):
                self._error(f"v2: 'include' is gone, use import: {line}")
                i += 1
            elif line.startswith('object '):
                self._error(f"v2: 'object' became 'class': {line}")
                i += 1
            elif re.match(r'^(public|private)\s+(static\s+)?class\s+', line) or line.startswith('class '):
                i = self._parse_class(i)
            elif re.match(r'^(public|private)\s+static\s+function\s+', line):
                self._error(f"'static' is only allowed inside a class: {line}")
                i += 1
            elif re.match(r'^(public|private)\s+function\s+', line):
                i = self._parse_function(i)
            elif line.startswith('function '):
                self._error(
                    f"v2: a function needs an access modifier: 'public function ...': {line}")
                i += 1
            elif self._top_decl(line):
                self._parse_global(line)
                i += 1
            elif re.match(r'^(number|string|color|array|num|str|col|arr)\s+', line):
                self._error(
                    f"v2: a declaration needs a modifier or local: 'public ...' / 'local ...': {line}")
                i += 1
            else:
                self.top.append(line)
                i += 1
        self._check_imports()
        return self.errors == 0

    def _top_decl(self, line):
        lst = self._decl_list(line)
        if not lst:
            return None
        if any(t not in TYPES and t not in self.objects for _v, _l, t, _n, _d in lst):
            return None
        if any(local for _v, local, _t, _n, _d in lst):
            return None  # a top level local is a statement, not a declaration
        if any(vis is None for vis, _l, _t, _n, _d in lst):
            return None
        return lst

    def _check_imports(self):
        for ns, path in self.imports.items():
            if path in STD_NAMESPACES:
                continue
            if ns not in self.objects:
                self._error(f"import {path}: no class '{ns}' among the sources")

    def _parse_class(self, i):
        m = re.match(r'^(?:(public|private)\s+)?class\s+(' + _NAME + r')\s*$', self.lines[i])
        if not m:
            self._error(f"invalid class header: {self.lines[i]}")
            return i + 1
        name = m.group(2)
        if name in self._parsed_classes:
            self._error(f"dup class '{name}'")
            return i + 1
        self._parsed_classes.add(name)
        fields = self.objects.setdefault(name, {})
        methods = {}
        j = i + 1
        while j < len(self.lines):
            line = self.lines[j]
            if line == 'end':
                self.objects[name] = fields
                self.methods[name] = methods
                return j + 1
            lst = self._decl_list(line)
            if lst and not any(local for _v, local, _t, _n, _d in lst):
                for vis, _loc, t, fn, v in lst:
                    if not vis:
                        self._error(f"class '{name}': a field needs a modifier "
                                    f"public/private: {line}")
                    elif t not in TYPES and t not in self.objects:
                        self._error(f"class '{name}': field '{fn}' needs a type, "
                                    f"built-in or class")
                    elif fn in fields:
                        self._error(f"dup field '{name}.{fn}'")
                    else:
                        fields[fn] = (vis, t, v)
                j += 1
                continue
            fm = re.match(
                r'^(public|private)\s+(static\s+)?function\s+(' + _NAME + r')'
                r'(?:\s*(\(.*\)|.*?))?(?:\s*->\s*(' + _NAME + r'))?\s*$', line)
            if fm:
                vis, static, mname = fm.group(1), bool(fm.group(2)), fm.group(3)
                params = self._parse_params(fm.group(4) or '')
                ret = canon_type(fm.group(5)) if fm.group(5) else None
                if ret == 'void':
                    ret = 'void'
                elif ret is not None and ret not in TYPES and ret not in self.objects \
                        and ret != name:
                    self._error(f"class '{name}': unknown return type '{ret}'")
                body, j = self._collect_block(j + 1, f"method '{name}.{mname}'")
                if mname in methods:
                    self._error(f"dup method '{name}.{mname}'")
                else:
                    methods[mname] = (vis, static, params, ret, body)
                if mname != 'new':
                    self._note_ret(name + '.' + mname, ret, body, params)
                continue
            if re.match(r'^function\s+', line):
                self._error(f"v2: a method needs a modifier: 'public function ...': {line}")
                j += 1
                continue
            self._error(f"class '{name}': expected a field or a method, got: {line}")
            j += 1
        self._error(f"class '{name}' no end")
        return j

    def _note_ret(self, key, ret, body, params):
        """Records the return type, either from '-> t' or inferred from return."""
        if ret and ret != 'void':
            self.func_ret[key] = ret
        elif ret == 'void':
            self.func_ret[key] = 'void'
        elif any(line.startswith('return ') for line in body):
            self.func_ret[key] = 'num'  # refined in _infer_returns
        else:
            self.func_ret[key] = 'void'

    def _parse_function(self, i):
        m = re.match(
            r'^(public|private)\s+function\s+(' + _NAME + r')'
            r'(?:\s*(\(.*\)|.*?))?(?:\s*->\s*(' + _NAME + r'))?\s*$', self.lines[i])
        vis, name = m.group(1), m.group(2)
        params = self._parse_params(m.group(3) or '')
        ret = canon_type(m.group(4)) if m.group(4) else None
        if ret == 'void':
            pass
        elif ret is not None and ret not in TYPES and ret not in self.objects:
            self._error(f"function '{name}': unknown return type '{ret}'")
        body, j = self._collect_block(i + 1, f"function '{name}'")
        if name in self.functions:
            self._error(f"dup function '{name}'")
        else:
            self.functions[name] = (vis, params, body)
        self._note_ret(name, ret, body, params)
        return j

    def _parse_params(self, text):
        text = (text or '').strip()
        if text.startswith('(') and text.endswith(')'):
            text = text[1:-1].strip()
        params = []
        for part in split_top(text, ',') if text.strip() else []:
            w = part.split()
            if len(w) != 2 or (canon_type(w[0]) not in TYPES and w[0] not in self.objects):
                self._error(f"invalid param '{part}' (expected 'type name')")
                continue
            params.append((canon_type(w[0]), w[1]))
        return params

    def _collect_block(self, i, what):
        depth = 0
        body = []
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                if depth == 0:
                    return body, i + 1
                depth -= 1
            elif line.startswith(('if ', 'while ', 'for ')):
                depth += 1
            elif line == 'else' or line.startswith(('else if ', 'elif ')):
                if depth == 0:
                    self._error(f"{what}: 'else' without 'if'")
                    return body, i + 1
            elif line.startswith(('class ', 'object ')) or re.match(
                    r'^(public|private)\s+(static\s+)?function\s+', line):
                self._error(f"{what}: nested declarations not allowed")
                body.append(line)
                i += 1
                continue
            body.append(line)
            i += 1
        self._error(f"{what} no end")
        return body, i

    def _parse_global(self, line):
        for vis, _loc, t, n, v in self._top_decl(line) or []:
            if n in self.vars:
                self._error(f"dup var '{n}'")
                continue
            if t in self.objects:
                if not v or not re.match(r'^new\s+' + re.escape(t) + r'\s*\(\)\s*$', v):
                    self._error(f"'{n}': must be 'new {t}()'")
                    continue
            self.vars[n] = (vis, t, v)
