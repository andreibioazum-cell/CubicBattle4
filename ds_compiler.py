#!/usr/bin/env python3
"""Compile DimScript source into typed C.

Script variables become typed C globals/locals (no hash-table lookup),
string ``+`` compiles to real ``ds_concat`` composition, ``for`` loops are
parsed into C, and ``object``/``class`` declarations become direct structs
with constructors, methods and destructors.
"""

from collections import OrderedDict
import os
import re
import sys


_INCLUDE_RE = re.compile(
    r'^(?:#\s*)?include\s*(?:"([^"]+)"|<([^>]+)>)\s*;?\s*$'
)
_INCLUDE_PREFIX_RE = re.compile(r'^(?:#\s*)?include\b')
_DECL_RE = re.compile(
    r'^(?P<type>[A-Za-z_][A-Za-z0-9_]*\*?)\s+'
    r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)'
    r'(?:\s*=\s*(?P<value>.*))?$'
)
_FN_RE = re.compile(
    r'^fn\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\((?P<params>.*)\)\s*(?P<brace>\{)?\s*$'
)
_OBJECT_RE = re.compile(
    r'^(?:object|class)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:\([^)]*\))?\s*(?P<brace>\{)?\s*$'
)
_NUMBER_RE = re.compile(r'^(?:[-+]?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?|0[xX][0-9a-fA-F]+)$')


class DimScriptCompiler:
    BUILTIN_TYPES = {
        'num': 'double', 'int': 'int', 'bool': 'int', 'str': 'const char *',
        'byte*': 'unsigned char *', 'size': 'size_t', 'col': 'uint32_t',
    }
    STRING_FUNCTIONS = {
        'ds_concat', 'ds_num_to_string', 'ds_bool_to_string',
    }
    NUMBER_FUNCTIONS = {
        'floor', 'ceil', 'round', 'sqrt', 'sin', 'cos', 'tan', 'atan2',
        'fabs', 'abs', 'rand', 'text_width', 'text_height',
        'ds_len', 'ds_find', 'ds_contains', 'ds_starts_with',
        'ds_ends_with',
    }

    @staticmethod
    def _scan(line):
        """Per-character (depth, in-string) state for splitting expressions."""
        length = len(line)
        depth = [0] * length
        quoted = [False] * length
        quote = None
        escaped = False
        level = 0
        for i, char in enumerate(line):
            quoted[i] = quote is not None
            if escaped:
                escaped = False
                continue
            if char == '\\' and quote:
                escaped = True
                continue
            if quote:
                if char == quote:
                    quote = None
                continue
            if char in ('"', "'"):
                quote = char
            elif char in '([{':
                level += 1
            elif char in ')]}':
                level = max(0, level - 1)
            depth[i] = level
        return depth, quoted

    @staticmethod
    def _strip_inline_comment(line):
        _, quoted = DimScriptCompiler._scan(line)
        for i in range(len(line) - 1):
            if not quoted[i] and line[i:i + 2] == '//':
                return line[:i]
        return line

    @staticmethod
    def _brace_counts(line):
        depth, quoted = DimScriptCompiler._scan(line)
        open_count = sum(1 for i, c in enumerate(line) if c == '{' and not quoted[i])
        close_count = sum(1 for i, c in enumerate(line) if c == '}' and not quoted[i])
        return open_count, close_count

    @staticmethod
    def _is_string_literal(value):
        value = value.strip()
        return len(value) >= 2 and value[0] == '"' and value[-1] == '"'

    @staticmethod
    def _strip_semicolon(value):
        value = value.rstrip()
        return value[:-1].rstrip() if value.endswith(';') else value

    def _error(self, message):
        self.errors += 1
        print(f"DimScript error: {message}", file=sys.stderr)

    def _load_source(self, path, included_from=None, include_line=0):
        canonical = os.path.realpath(os.path.abspath(os.fspath(path)))
        if canonical in self._include_stack:
            start = self._include_stack.index(canonical)
            cycle = self._include_stack[start:] + [canonical]
            self._error("cyclic include: " + " -> ".join(os.path.basename(p) for p in cycle))
            return False
        if canonical in self._loaded_paths:
            return True
        if len(self._include_stack) >= 128:
            self._error(f"include nesting is too deep near '{canonical}'")
            return False
        if not os.path.isfile(canonical):
            where = f"{included_from}:{include_line}: " if included_from else ""
            self._error(f"{where}source file not found: {path}")
            return False

        self._include_stack.append(canonical)
        self.loaded_sources.append(canonical)
        success = True
        try:
            with open(canonical, 'r', encoding='utf-8-sig') as source:
                for line_number, raw in enumerate(source, 1):
                    line = self._strip_inline_comment(raw).strip()
                    if not line:
                        continue
                    include = _INCLUDE_RE.fullmatch(line)
                    if include:
                        include_name = include.group(1) or include.group(2)
                        include_path = os.path.join(os.path.dirname(canonical), include_name)
                        if not self._load_source(include_path, canonical, line_number):
                            success = False
                        continue
                    if _INCLUDE_PREFIX_RE.match(line):
                        self._error(f"{canonical}:{line_number}: malformed include; "
                                    'use #include "file.ds"')
                        success = False
                        continue
                    self.src.append(line)
        except (OSError, UnicodeError) as error:
            self._error(f"cannot read '{canonical}': {error}")
            success = False
        finally:
            self._include_stack.pop()
        if success:
            self._loaded_paths.add(canonical)
        return success

    def parse(self, paths):
        self.vars = OrderedDict()
        self.functions = OrderedDict()
        self.objects = OrderedDict()
        self.main_body = []
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

        for path in paths:
            if not self._load_source(path):
                return False

        i = 0
        while i < len(self.src):
            line = self.src[i]
            if _OBJECT_RE.match(line):
                i = self.parse_object(i)
            elif _FN_RE.match(line):
                i = self.parse_func(i)
            elif 'Main(' in line:
                i = self.parse_main(i)
            elif self._looks_like_declaration(line):
                self.parse_var(line)
                i += 1
            else:
                i += 1  # unknown top-level lines are ignored
        self._collect_object_variables()
        return self.errors == 0

    def _looks_like_declaration(self, line):
        match = _DECL_RE.match(self._strip_semicolon(line.strip()))
        if not match:
            return False
        type_name = match.group('type')
        return type_name in self.BUILTIN_TYPES or type_name.rstrip('*') in self.objects

    @staticmethod
    def _parse_params(params_text):
        params = []
        for part in DimScriptCompiler._split_top_level(params_text, ','):
            part = part.strip()
            if not part:
                continue
            words = part.split()
            params.append(('num', words[0]) if len(words) == 1 else (words[0], words[-1]))
        return params

    def parse_var(self, line, target=None):
        match = _DECL_RE.match(self._strip_semicolon(line.strip()))
        if not match:
            self._error(f"invalid variable declaration: {line}")
            return None
        vtype, name, value = match.group('type'), match.group('name'), match.group('value')
        target = self.vars if target is None else target
        if name in target:
            self._error(f"duplicate variable '{name}'")
        else:
            target[name] = (vtype, value.strip() if value is not None else None)
        return name, vtype, value.strip() if value is not None else None

    def _extract_block(self, lines, start, header):
        """Return (body, index after closing brace).  A ``} else {`` line is
        kept in the body so compile_line can emit the C transition."""
        opening, closing = self._brace_counts(header)
        depth = opening - closing
        i = start + 1
        body = []
        if depth <= 0:
            if i < len(lines) and lines[i].strip() == '{':  # ``fn f()`` + ``{``
                depth = 1
                i += 1
            else:
                return body, i
        while i < len(lines) and depth > 0:
            current = lines[i]
            op, cl = self._brace_counts(current)
            if depth == 1 and current.strip() == '}' and op == 0 and cl == 1:
                i += 1
                break
            body.append(current)
            depth += op - cl
            i += 1
        return body, i

    def parse_func(self, i):
        match = _FN_RE.match(self.src[i])
        if not match:
            self._error(f"invalid function declaration: {self.src[i]}")
            return i + 1
        name = match.group('name')
        params = self._parse_params(match.group('params'))
        body, next_index = self._extract_block(self.src, i, self.src[i])
        depth = sum(a - b for a, b in map(self._brace_counts, self.src[i:next_index]))
        if depth != 0:
            self._error(f"function '{name}' has no closing '}}'")
        elif name in self.functions:
            self._error(f"duplicate function '{name}'")
        else:
            self.functions[name] = (params, body)
        return next_index

    def parse_object(self, i):
        match = _OBJECT_RE.match(self.src[i])
        if not match:
            self._error(f"invalid object declaration: {self.src[i]}")
            return i + 1
        name = match.group('name')
        lines, next_index = self._extract_block(self.src, i, self.src[i])
        fields = OrderedDict()
        methods = OrderedDict()
        j = 0
        while j < len(lines):
            line = lines[j]
            fn_match = _FN_RE.match(line)
            if fn_match:
                method_name = fn_match.group('name')
                params = self._parse_params(fn_match.group('params'))
                body, after = self._extract_block(lines, j, line)
                if method_name in methods:
                    self._error(f"duplicate method '{name}.{method_name}'")
                else:
                    methods[method_name] = (params, body)
                j = after
                continue
            if self._looks_like_declaration(line):
                parsed = self.parse_var(line, fields)
                if parsed and parsed[1].rstrip('*') in self.objects:
                    self._error(f"nested object fields are not supported yet: {name}.{parsed[0]}")
                j += 1
                continue
            if line not in ('{', '}'):
                self._error(f"unknown line in object '{name}': {line}")
            j += 1
        if name in self.objects:
            self._error(f"duplicate object '{name}'")
        else:
            self.objects[name] = {'fields': fields, 'methods': methods}
        return next_index

    def parse_main(self, i):
        body, next_index = self._extract_block(self.src, i, self.src[i])
        self.main_body = body
        return next_index

    def _collect_object_variables(self):
        self.object_vars = {
            name: vtype.rstrip('*')
            for name, (vtype, _value) in self.vars.items()
            if vtype.rstrip('*') in self.objects
        }

    @staticmethod
    def _split_top_level(text, delimiter):
        parts = []
        start = 0
        depth, quoted = DimScriptCompiler._scan(text)
        for i, char in enumerate(text):
            if depth[i] == 0 and not quoted[i] and char == delimiter:
                parts.append(text[start:i].strip())
                start = i + 1
        parts.append(text[start:].strip())
        return parts

    @classmethod
    def _split_plus(cls, expression):
        parts = cls._split_top_level(expression, '+')
        if len(parts) <= 1 or any(not part for part in parts):
            return None  # reject ++ or a leading unary +
        return parts

    @staticmethod
    def _strip_outer_parens(expression):
        expression = expression.strip()
        while expression.startswith('(') and expression.endswith(')'):
            _, quoted = DimScriptCompiler._scan(expression)
            depth = 0
            ok = True
            for i in range(len(expression)):
                if quoted[i]:
                    continue
                if expression[i] == '(':
                    depth += 1
                elif expression[i] == ')':
                    depth -= 1
                    if depth == 0 and i != len(expression) - 1:
                        ok = False
                        break
            if not ok or depth != 0:
                break
            expression = expression[1:-1].strip()
        return expression

    @staticmethod
    def _whole_call(expression):
        expression = expression.strip()
        match = re.match(r'^([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?)\s*\(', expression)
        if not match:
            return None
        _, quoted = DimScriptCompiler._scan(expression)
        open_at = expression.find('(', match.start())
        depth = 0
        for i in range(open_at, len(expression)):
            if quoted[i]:
                continue
            if expression[i] == '(':
                depth += 1
            elif expression[i] == ')':
                depth -= 1
                if depth == 0:
                    if i == len(expression) - 1:
                        return match.group(1), expression[open_at + 1:i]
                    return None
        return None

    def _symbol_type(self, name):
        name = name.strip()
        if name in self.current_scope:
            return self.current_scope[name]
        if name in self.vars:
            return self.vars[name][0]
        if name in self.object_vars:
            return self.object_vars[name]
        field_match = re.match(r'^([A-Za-z_]\w*)\s*(?:\.|->)\s*([A-Za-z_]\w*)$', name)
        if field_match:
            owner, field = field_match.groups()
            object_type = self.current_object if (owner == 'self' and self.current_object) else (
                self.current_scope.get(owner) or self.object_vars.get(owner))
            if object_type in self.objects:
                field_info = self.objects[object_type]['fields'].get(field)
                if field_info:
                    return field_info[0]
        if name in ('true', 'false'):
            return 'bool'
        if name.startswith('"'):
            return 'str'
        if name in self.STRING_FUNCTIONS:
            return 'str'
        return None

    def infer_expr_type(self, expression):
        expression = self._strip_outer_parens(expression.strip())
        if not expression:
            return None
        if self._is_string_literal(expression):
            return 'str'
        plus_parts = self._split_plus(expression)
        if plus_parts:
            return 'str' if any(self.infer_expr_type(p) == 'str' for p in plus_parts) else 'num'
        if re.search(r'(^|\s)(==|!=|<=|>=|<|>)(\s|$)', expression):
            return 'bool'
        call = self._whole_call(expression)
        if call:
            bare = call[0].split('.')[-1]
            if bare in self.STRING_FUNCTIONS:
                return 'str'
            if bare in self.NUMBER_FUNCTIONS or bare in self.functions:
                return 'num'
        symbol = self._symbol_type(expression)
        if symbol:
            return symbol
        return 'num'

    def _translate_object_access(self, expression):
        expression = re.sub(r'\btrue\b', '1', expression)
        expression = re.sub(r'\bfalse\b', '0', expression)
        known_objects = dict(self.object_vars)
        for variable, script_type in self.current_scope.items():
            if script_type.rstrip('*') in self.objects:
                known_objects[variable] = script_type.rstrip('*')
        for variable, object_type in sorted(known_objects.items(), key=lambda item: -len(item[0])):
            expression = re.sub(rf'\b{re.escape(variable)}\.([A-Za-z_]\w*)',
                                rf'{variable}->\1', expression)
        if self.current_object:
            expression = re.sub(r'\bself\.([A-Za-z_]\w*)', r'self->\1', expression)
        return expression

    def _as_string_expression(self, expression):
        expression = self.translate_expression(expression)
        expr_type = self.infer_expr_type(expression)
        if expr_type == 'str':
            return expression
        if expr_type == 'bool':
            return f'ds_bool_to_string((int)({expression}))'
        return f'ds_num_to_string((double)({expression}))'

    def translate_expression(self, expression):
        expression = expression.strip()
        if not expression:
            return expression
        stripped = self._strip_outer_parens(expression)
        plus_parts = self._split_plus(stripped)
        if plus_parts:
            types = [self.infer_expr_type(part) for part in plus_parts]
            if any(value_type == 'str' for value_type in types):
                result = self._as_string_expression(plus_parts[0])
                for part in plus_parts[1:]:
                    result = f'ds_concat({result}, {self._as_string_expression(part)})'
                return result
            return self._translate_object_access(expression)
        call = self._whole_call(stripped)
        if call:
            name, args_text = call
            args = [self.translate_expression(arg)
                    for arg in self._split_top_level(args_text, ',')]
            return self._translate_object_access(f'{name}({", ".join(args)})')
        return self._translate_object_access(expression)

    def c_type(self, script_type):
        if script_type in self.BUILTIN_TYPES:
            return self.BUILTIN_TYPES[script_type]
        object_type = script_type.rstrip('*')
        return f'{object_type} *' if object_type in self.objects else script_type

    def _default_c_value(self, script_type, value=None):
        if value is not None and value.strip():
            return self.translate_expression(value)
        if script_type in ('str',) or script_type.endswith('*') or script_type.rstrip('*') in self.objects:
            return 'NULL'
        return '0'

    def _is_c_static_expression(self, value):
        value = value.strip()
        return (self._is_string_literal(value) or _NUMBER_RE.match(value)
                or value in ('NULL', 'true', 'false'))

    def generate(self):
        self.output = []
        self.emit('#include "runtime.h"')
        self.emit('#include <math.h>')
        self.emit('')

        for name in self.objects:
            self.emit(f'typedef struct {name} {name};')
        if self.objects:
            self.emit('')
        for name, definition in self.objects.items():
            self.emit(f'struct {name} {{')
            fields = definition['fields']
            if not fields:
                self.emit('    unsigned char _ds_empty;')
            for field, (field_type, _value) in fields.items():
                self.emit(f'    {self.c_type(field_type)} {field};')
            self.emit('};')
        if self.objects:
            self.emit('')

        for name, definition in self.objects.items():
            for method, (params, _body) in definition['methods'].items():
                self.emit(f'{self._object_signature(name, method, params)};')
            self.emit(f'static {name} *ds_new_{name}(' +
                      self._params_c(params_for_new(definition)) + ');')
            self.emit(f'static void ds_free_{name}({name} *self);')
        if self.objects:
            self.emit('')

        for name, (script_type, value) in self.vars.items():
            c_type = self.c_type(script_type)
            if script_type.rstrip('*') in self.objects:
                initial = 'NULL'
                if value:
                    self.global_initializers.append((name, value))
            else:
                string_needs_conversion = (
                    script_type == 'str' and value is not None and value.strip() and
                    self.infer_expr_type(value) != 'str')
                if value is not None and value.strip() and self._is_c_static_expression(value) and not string_needs_conversion:
                    initial = self.translate_expression(value)
                else:
                    initial = self._default_c_value(script_type)
                    if value:
                        self.global_initializers.append((name, value))
            self.emit(f'{c_type} {name} = {initial};')
        if self.vars:
            self.emit('')

        for name, (params, _body) in self.functions.items():
            self.emit(f'static void ds_fn_{name}({self._params_c(params)});')
        if self.functions:
            self.emit('')

        for name, definition in self.objects.items():
            for method, (params, body) in definition['methods'].items():
                self.emit(self._object_signature(name, method, params) + ' {')
                method_scope = {'self': name}
                method_scope.update({p: t for t, p in params})
                self._emit_body(body, method_scope, name, method)
                self.emit('}')
                self.emit('')
            self.emit(f'static {name} *ds_new_{name}(' + self._params_c(params_for_new(definition)) + ') {')
            self.emit(f'    {name} *self = ({name} *)calloc(1, sizeof(*self));')
            self.emit(f'    if (!self) {{ ds_runtime_error(\\"out of memory creating object {name}\\"); return NULL; }}')
            for field, (field_type, value) in definition['fields'].items():
                if value is not None and value.strip():
                    field_value = (self._as_string_expression(value)
                                   if field_type == 'str' and self.infer_expr_type(value) != 'str'
                                   else self.translate_expression(value))
                    self.emit(f'    self->{field} = {field_value};')
            if 'init' in definition['methods']:
                args = ', '.join(p for _t, p in params_for_new(definition))
                call = f'ds_obj_{name}_init(self' + (f', {args}' if args else '') + ');'
                self.emit(f'    {call}')
            self.emit('    return self;')
            self.emit('}')
            self.emit(f'static void ds_free_{name}({name} *self) {{')
            self.emit('    free(self);')
            self.emit('}')
            self.emit('')

        for name, (params, body) in self.functions.items():
            self.emit(f'static void ds_fn_{name}({self._params_c(params)}) {{')
            function_scope = {p: t for t, p in params}
            self._emit_body(body, function_scope, None, name)
            self.emit('}')
            self.emit('')

        self.emit('static int ds_main(void) {')
        for name, value in self.global_initializers:
            self.compile_line(f'{name} = {value}')
        for line in self.main_body:
            self.compile_line(line)
        self.emit('    return 0;')
        self.emit('}')
        self.emit('')

        self.emit('void reset(void) {')
        for name, (script_type, value) in self.vars.items():
            if script_type.rstrip('*') in self.objects:
                self.emit(f'    if ({name}) ds_free_{script_type.rstrip("*")}({name});')
                self.emit(f'    {name} = NULL;')
            elif value is not None and self._is_c_static_expression(value):
                self.emit(f'    {name} = {self.translate_expression(value)};')
            else:
                self.emit(f'    {name} = {self._default_c_value(script_type)};')
        self.emit('}')
        self.emit('')

        self.emit('void init(AAssetManager *assets) {')
        self.emit('    ds_set_asset_manager(assets);')
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
        else:
            self.emit('    (void)x; (void)y; (void)action;')
        self.emit('}')

    def _params_c(self, params):
        if not params:
            return 'void'
        return ', '.join(f'{self.c_type(t)} {n}' for t, n in params)

    def _object_signature(self, object_name, method_name, params):
        params_c = ', '.join([f'{object_name} *self'] +
                             [f'{self.c_type(t)} {n}' for t, n in params])
        return f'static void ds_obj_{object_name}_{method_name}({params_c})'

    def _emit_body(self, body, params, object_name, function_name):
        previous_scope = self.current_scope
        previous_object = self.current_object
        previous_function = self.current_function
        self.current_scope = dict(params)
        self.current_object = object_name
        self.current_function = function_name
        # Only suppress warnings for parameters that are actually unused. A
        # parameter that appears in the body needs no (void) cast, and emitting
        # one before every body is dead, misleading code.
        body_text = '\n'.join(body)
        for parameter in params:
            if not self._text_uses_identifier(body_text, parameter):
                self._indent(f'(void){parameter};')
        for line in body:
            self.compile_line(line)
        self.current_scope = previous_scope
        self.current_object = previous_object
        self.current_function = previous_function

    @staticmethod
    def _text_uses_identifier(text, identifier):
        """True if identifier appears as a whole word outside string literals.

        Matches only standalone tokens (word boundaries on both sides), so a
        parameter named ``x`` is not reported as used by ``btn_x`` or by the
        same letters inside a quoted string.
        """
        pattern = re.compile(r'\b' + re.escape(identifier) + r'\b')
        _, quoted = DimScriptCompiler._scan(text)
        return any(not quoted[match.start()] for match in pattern.finditer(text))

    def _indent(self, line):
        self.emit(f'    {line}')

    def compile_line(self, raw_line):
        line = self._strip_semicolon(raw_line.strip())
        if not line:
            return

        declaration = _DECL_RE.match(line)
        if declaration and (declaration.group('type') in self.BUILTIN_TYPES or
                            declaration.group('type').rstrip('*') in self.objects):
            self._compile_declaration(declaration, line)
            return

        if line.startswith('for'):
            match = re.match(r'^for\s*\((.*)\)\s*\{?$', line)
            if match:
                parts = self._split_top_level(match.group(1), ';')
                if len(parts) == 3:
                    init, condition, step = (p.strip() for p in parts)
                    self._indent(f'for ({self._translate_for_init(init)}; '
                                 f'{self.translate_expression(condition)}; '
                                 f'{self.translate_expression(step)}) {{')
                    return
            self._error(f"invalid for loop: {raw_line}")
            return

        for keyword, offset in (('if', 2), ('while', 5), ('loop', 5)):
            if line.startswith(keyword + ' '):
                condition = line[offset:].strip()
                if condition.endswith('{'):
                    condition = condition[:-1].strip()
                c_keyword = 'while' if keyword == 'loop' else keyword
                self._indent(f'{c_keyword} ({self.translate_expression(self._strip_outer_parens(condition))}) {{')
                return

        if line == 'else' or line == 'else {':
            self._indent('else {')
            return
        if line.startswith('else if '):
            condition = line[8:].strip()
            if condition.endswith('{'):
                condition = condition[:-1].strip()
            self._indent(f'else if ({self.translate_expression(self._strip_outer_parens(condition))}) {{')
            return

        if line.startswith('return') and (line == 'return' or line[6:7] in (' ', ';')):
            value = line[6:].strip()
            self._indent(f'return {self.translate_expression(value)};' if value else 'return;')
            return

        new_assignment = re.match(
            r'^([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_]\w*)\s*(?:\((.*)\))?$', line)
        if new_assignment and new_assignment.group(2) in self.objects:
            variable, object_type = new_assignment.group(1), new_assignment.group(2)
            self._indent(f'{variable} = ds_new_{object_type}({self._translate_args(new_assignment.group(3) or "")});')
            self.current_scope[variable] = object_type
            return

        if line.startswith('delete '):
            variable = line[7:].strip()
            object_type = self.current_scope.get(variable) or self.object_vars.get(variable)
            self._indent(f'ds_free_{object_type}({variable}); {variable} = NULL;'
                         if object_type in self.objects
                         else f'free({variable}); {variable} = NULL;')
            return

        if line.startswith('}'):
            self._indent('}')
            rest = line[1:].strip()
            if rest:
                self.compile_line(rest)
            return
        if line.endswith('{'):
            self._indent(line)
            return

        call = self._whole_call(line)
        if call:
            self._compile_call(call[0], call[1])
            return

        if self._find_assignment(line):
            self.compile_assign(line)
            return

        self._indent(self.translate_expression(line) + ';')

    def _compile_declaration(self, declaration, line):
        script_type = declaration.group('type')
        name = declaration.group('name')
        value = declaration.group('value')
        if script_type.rstrip('*') in self.objects:
            object_type = script_type.rstrip('*')
            self.current_scope[name] = object_type
            new_match = re.match(r'new\s+([A-Za-z_]\w*)\s*(?:\((.*)\))?$', value.strip()) if value and value.strip().startswith('new ') else None
            if new_match and new_match.group(1) == object_type:
                args = self._translate_args(new_match.group(2) or '')
                self._indent(f'{object_type} *{name} = ds_new_{object_type}({args});')
            else:
                self._indent(f'{object_type} *{name} = NULL;')
            return
        self.current_scope[name] = script_type
        c_line = f'{self.c_type(script_type)} {name}'
        if value is not None and value.strip():
            translated_value = (self._as_string_expression(value)
                                if script_type == 'str' and self.infer_expr_type(value) != 'str'
                                else self.translate_expression(value))
            c_line += f' = {translated_value}'
        self._indent(c_line + ';')

    def _translate_for_init(self, init):
        declaration = _DECL_RE.match(init)
        if declaration and declaration.group('type') in self.BUILTIN_TYPES:
            script_type, name, value = (declaration.group('type'),
                                        declaration.group('name'), declaration.group('value'))
            self.current_scope[name] = script_type
            result = f'{self.c_type(script_type)} {name}'
            if value:
                result += f' = {self.translate_expression(value)}'
            return result
        return self.translate_expression(init)

    def _translate_args(self, args_text):
        if not args_text.strip():
            return ''
        return ', '.join(self.translate_expression(arg)
                         for arg in self._split_top_level(args_text, ','))

    def _compile_call(self, name, args_text):
        args = self._translate_args(args_text)
        bare_name = name.split('.')[-1]
        if '.' in name:
            object_name, method = name.split('.', 1)
            object_type = self.current_scope.get(object_name) or self.object_vars.get(object_name)
            if object_type in self.objects and method in self.objects[object_type]['methods']:
                call = f'ds_obj_{object_type}_{method}({object_name}'
                if args:
                    call += f', {args}'
                self._indent(call + ');')
                return
        if bare_name in self.functions:
            self._indent(f'ds_fn_{bare_name}({args});')
        else:
            self._indent(f'{self._translate_object_access(name)}({args});')

    @staticmethod
    def _find_assignment(line):
        depth, quoted = DimScriptCompiler._scan(line)
        for i, char in enumerate(line):
            if quoted[i] or depth[i]:
                continue
            if char == '=' and (i == 0 or line[i - 1] not in '<>!=') and \
                    (i + 1 >= len(line) or line[i + 1] != '='):
                return True
            if char in '+-*/' and i + 1 < len(line) and line[i + 1] == '=':
                return True
        return False

    @staticmethod
    def _assignment_parts(line):
        match = re.match(r'^(.+?)\s*(\+=|-=|\*=|/=|=)\s*(.*)$', line)
        return match.groups() if match else None

    def compile_assign(self, line):
        parts = self._assignment_parts(line)
        if not parts:
            self._indent(self.translate_expression(line) + ';')
            return
        left, operator, right = (p.strip() for p in parts)
        left_type = self.infer_expr_type(left)
        right_type = self.infer_expr_type(right)
        left = self._translate_object_access(left)
        if operator == '+=' and (left_type == 'str' or right_type == 'str'):
            self._indent(f'{left} = ds_concat({left}, {self._as_string_expression(right)});')
            return
        if operator == '=' and left_type == 'str' and right_type != 'str':
            self._indent(f'{left} = {self._as_string_expression(right)};')
            return
        self._indent(f'{left} {operator} {self.translate_expression(right)};')

    def emit(self, line):
        self.output.append(line)

    def compile(self, sources, output):
        if not self.parse(sources):
            return False
        self.generate()
        if self.errors == 0:
            with open(output, 'w', encoding='utf-8') as generated:
                generated.write('\n'.join(self.output) + '\n')
            return True
        return False


def params_for_new(definition):
    """Constructor parameters match the user-defined init method, if any."""
    init = definition['methods'].get('init')
    return init[0] if init else []


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
    compiler = DimScriptCompiler()
    sys.exit(0 if compiler.compile(sources, output) else 1)


if __name__ == '__main__':
    main()
