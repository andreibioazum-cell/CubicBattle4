#!/usr/bin/env python3
"""
DimScript Compiler - Python implementation
Converts .ds files into C code for the DimScript runtime.
"""

import sys
import os
import re
from dataclasses import dataclass
from typing import List, Dict, Optional, Tuple

# Constants
MAX_LINE = 4096
MAX_SOURCE = 4096
MAX_FUNCTIONS = 128
MAX_PARAMS = 16
MAX_LOCALS = 64
MAX_NAME = 64
DS_TABLE_SIZE = 128

# Built-in functions
BUILTIN_FUNCTIONS = {
    'cls', 'rect', 'circle', 'ring', 'tex', 'text',
    'print', 'printn', 'prints', 'sqrt', 'sin', 'cos', 'atan2'
}

@dataclass
class SourceLine:
    text: str
    source_name: str
    source_line: int

@dataclass
class Function:
    name: str
    params: List[str]
    body_start: int
    body_end: int

class Compiler:
    def __init__(self):
        self.source_lines: List[SourceLine] = []
        self.functions: List[Function] = []
        self.locals: List[str] = []
        self.output: List[str] = []
        self.errors: int = 0
        self.line_number: int = 0
        self.source_name: str = ""
        self.table_number: int = 0
        self.block_depth: int = 0

    # ----------------------------------------------------------------------
    # 1. Source loading and preprocessing
    # ----------------------------------------------------------------------

    def load_source(self, path: str) -> bool:
        """Load a .ds file, strip comments and blank lines."""
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for line_num, raw in enumerate(f, 1):
                    line = self.remove_comments(raw)
                    line = line.strip()
                    if line:  # skip empty lines
                        self.source_lines.append(SourceLine(
                            text=line,
                            source_name=path,
                            source_line=line_num
                        ))
            return True
        except Exception as e:
            print(f"Error: cannot open {path}: {e}", file=sys.stderr)
            return False

    def remove_comments(self, line: str) -> str:
        """Remove -- and // comments, respecting string literals."""
        result = []
        in_string = False
        escaped = False
        i = 0
        while i < len(line):
            ch = line[i]
            if in_string:
                if escaped:
                    escaped = False
                elif ch == '\\':
                    escaped = True
                elif ch == '"':
                    in_string = False
                result.append(ch)
            else:
                if ch == '"':
                    in_string = True
                    result.append(ch)
                elif ch == '-' and i+1 < len(line) and line[i+1] == '-':
                    break
                elif ch == '/' and i+1 < len(line) and line[i+1] == '/':
                    break
                else:
                    result.append(ch)
            i += 1
        return ''.join(result)

    # ----------------------------------------------------------------------
    # 2. Lexical helpers
    # ----------------------------------------------------------------------

    @staticmethod
    def is_identifier_start(ch: str) -> bool:
        return ch.isalpha() or ch == '_'

    @staticmethod
    def is_identifier_part(ch: str) -> bool:
        return ch.isalnum() or ch == '_'

    @staticmethod
    def valid_identifier(name: str) -> bool:
        if not name or not Compiler.is_identifier_start(name[0]):
            return False
        return all(Compiler.is_identifier_part(ch) for ch in name[1:])

    @staticmethod
    def starts_keyword(line: str, keyword: str) -> bool:
        if not line.startswith(keyword):
            return False
        after = line[len(keyword):]
        return (not after or after[0].isspace() or after[0] == '(')

    @staticmethod
    def block_starts(line: str) -> bool:
        return (Compiler.starts_keyword(line, 'if') or
                Compiler.starts_keyword(line, 'while') or
                Compiler.starts_keyword(line, 'for') or
                Compiler.starts_keyword(line, 'function'))

    # ----------------------------------------------------------------------
    # 3. Function parsing
    # ----------------------------------------------------------------------

    def parse_function_header(self, line: str) -> Optional[Function]:
        if not self.starts_keyword(line, 'function'):
            return None

        rest = line[8:].strip()  # remove 'function'
        open_paren = rest.find('(')
        if open_paren == -1:
            self.error("function declaration must look like function name(args)")
            return None

        name = rest[:open_paren].strip()
        if not self.valid_identifier(name):
            self.error(f"invalid function name '{name}'")
            return None

        close_paren = rest.rfind(')')
        if close_paren == -1 or close_paren < open_paren:
            self.error("function declaration must look like function name(args)")
            return None

        params_str = rest[open_paren+1:close_paren].strip()
        params = []
        if params_str:
            for p in params_str.split(','):
                p = p.strip()
                if not self.valid_identifier(p):
                    self.error(f"invalid parameter '{p}' in function '{name}'")
                    return None
                params.append(p)
                if len(params) > MAX_PARAMS:
                    self.error(f"too many parameters in function '{name}'")
                    return None

        return Function(name=name, params=params, body_start=0, body_end=0)

    def collect_functions(self) -> bool:
        i = 0
        while i < len(self.source_lines):
            line = self.source_lines[i]
            self.set_position(line)
            func = self.parse_function_header(line.text)
            if func is None:
                i += 1
                continue

            # Find body
            body_start = i + 1
            depth = 1
            body_end = -1
            for j in range(i+1, len(self.source_lines)):
                text = self.source_lines[j].text
                if self.block_starts(text):
                    depth += 1
                elif text == 'end':
                    depth -= 1
                    if depth == 0:
                        body_end = j
                        break

            if body_end == -1:
                self.error(f"function '{func.name}' has no matching end")
                return False

            # Check duplicate
            for existing in self.functions:
                if existing.name == func.name:
                    self.error(f"function '{func.name}' is defined more than once")
                    return False

            func.body_start = body_start
            func.body_end = body_end
            self.functions.append(func)
            i = body_end + 1

        return self.errors == 0

    # ----------------------------------------------------------------------
    # 4. Error reporting
    # ----------------------------------------------------------------------

    def set_position(self, line: SourceLine):
        self.source_name = line.source_name
        self.line_number = line.source_line

    def error(self, msg: str):
        self.errors += 1
        print(f"{self.source_name}:{self.line_number}: error: {msg}", file=sys.stderr)

    # ----------------------------------------------------------------------
    # 5. Code generation helpers
    # ----------------------------------------------------------------------

    def emit(self, line: str = '', indent: int = 0):
        if indent:
            self.output.append('    ' * indent + line)
        else:
            self.output.append(line)

    def function_index(self, name: str) -> int:
        for i, f in enumerate(self.functions):
            if f.name == name:
                return i
        return -1

    def is_local(self, name: str) -> bool:
        return name in self.locals

    def add_local(self, name: str) -> bool:
        if name in self.locals:
            return True
        if len(self.locals) >= MAX_LOCALS:
            self.error("too many local variables")
            return False
        self.locals.append(name)
        return True

    # ----------------------------------------------------------------------
    # 6. Expression emission
    # ----------------------------------------------------------------------

    def emit_expression(self, expr: str) -> str:
        result = []
        i = 0
        while i < len(expr):
            ch = expr[i]

            # String literal
            if ch == '"':
                result.append(ch)
                i += 1
                while i < len(expr):
                    result.append(expr[i])
                    if expr[i] == '\\':
                        i += 1
                        if i < len(expr):
                            result.append(expr[i])
                    elif expr[i] == '"':
                        break
                    i += 1
                i += 1
                continue

            # Number literal
            if ch.isdigit() or (ch == '.' and i+1 < len(expr) and expr[i+1].isdigit()):
                num = ''
                while i < len(expr) and (expr[i].isdigit() or expr[i] == '.'):
                    num += expr[i]
                    i += 1
                result.append(num)
                continue

            # Identifier
            if self.is_identifier_start(ch):
                name = ''
                while i < len(expr) and self.is_identifier_part(expr[i]):
                    name += expr[i]
                    i += 1

                # Logical keywords
                if name == 'and':
                    result.append(' && ')
                    continue
                if name == 'or':
                    result.append(' || ')
                    continue
                if name == 'not':
                    result.append(' !')
                    continue

                # Function call
                if i < len(expr) and expr[i] == '(':
                    if name not in BUILTIN_FUNCTIONS and self.function_index(name) < 0:
                        self.error(f"unknown function '{name}'")
                    if self.function_index(name) >= 0:
                        result.append(f'ds_fn_{name}')
                    else:
                        result.append(name)
                    result.append('(')
                    i += 1
                    continue

                # Member access
                if i < len(expr) and expr[i] == '.':
                    i += 1
                    member = ''
                    while i < len(expr) and self.is_identifier_part(expr[i]):
                        member += expr[i]
                        i += 1
                    result.append(self.emit_identifier(name, member))
                else:
                    result.append(self.emit_identifier(name, None))
                continue

            # Otherwise just output character
            result.append(ch)
            i += 1

        return ''.join(result)

    def emit_identifier(self, name: str, member: Optional[str]) -> str:
        if member is not None:
            if name == 'joy' and member in ('x', 'y', 'dx', 'dy', 'ox', 'oy', 'r'):
                return f'joy.{member}'
            return f'ds_read_field("{name}", "{member}")'

        if self.is_local(name):
            return name
        if name in ('screen_w', 'screen_h', 'fps'):
            return name
        if name == 'true':
            return '1.0'
        if name in ('false', 'nil'):
            return '0.0'
        return f'ds_read("{name}")'

    # ----------------------------------------------------------------------
    # 7. Statement compilation
    # ----------------------------------------------------------------------

    def split_statements(self, line: str) -> List[str]:
        """Split line by ';' respecting strings and brackets."""
        result = []
        current = []
        in_string = False
        escaped = False
        depth = 0
        for ch in line:
            if in_string:
                if escaped:
                    escaped = False
                elif ch == '\\':
                    escaped = True
                elif ch == '"':
                    in_string = False
                current.append(ch)
            else:
                if ch == '"':
                    in_string = True
                    current.append(ch)
                elif ch in '([{':
                    depth += 1
                    current.append(ch)
                elif ch in ')]}':
                    if depth > 0:
                        depth -= 1
                    current.append(ch)
                elif ch == ';' and depth == 0:
                    result.append(''.join(current).strip())
                    current = []
                else:
                    current.append(ch)
        if current:
            result.append(''.join(current).strip())
        return result

    def compile_statement_list(self, raw_line: str):
        stmts = self.split_statements(raw_line)
        for stmt in stmts:
            if stmt:
                self.compile_statement(stmt)

    def compile_statement(self, line: str):
        if self.starts_keyword(line, 'if'):
            self.compile_if(line)
        elif self.starts_keyword(line, 'elseif'):
            self.compile_elseif(line)
        elif line == 'else':
            self.compile_else()
        elif self.starts_keyword(line, 'while'):
            self.compile_while(line)
        elif self.starts_keyword(line, 'for'):
            self.compile_for(line)
        elif self.starts_keyword(line, 'local'):
            self.compile_local(line)
        elif self.starts_keyword(line, 'return'):
            self.compile_return(line)
        elif line == 'break' or line == 'continue':
            self.compile_break_continue(line)
        elif line == 'end':
            self.compile_end()
        elif '(' in line and line.endswith(')'):
            self.compile_call(line)
        else:
            self.compile_assignment(line)

    # ----------------------------------------------------------------------
    # 8. Specific statement compilers
    # ----------------------------------------------------------------------

    def compile_if(self, line: str):
        cond = line[2:].strip()  # remove 'if'
        if ' then' in cond:
            cond = cond.split(' then')[0]
        else:
            self.error("if statement must end with 'then'")
            return
        self.emit(f'if ({self.emit_expression(cond)}) {{', indent=self.block_depth)
        self.block_depth += 1

    def compile_elseif(self, line: str):
        cond = line[6:].strip()  # remove 'elseif'
        if ' then' in cond:
            cond = cond.split(' then')[0]
        else:
            self.error("elseif statement must end with 'then'")
            return
        if self.block_depth <= 0:
            self.error("invalid elseif statement")
            return
        self.emit(f'}} else if ({self.emit_expression(cond)}) {{', indent=self.block_depth-1)

    def compile_else(self):
        if self.block_depth <= 0:
            self.error("else without matching if")
            return
        self.emit('} else {', indent=self.block_depth-1)

    def compile_while(self, line: str):
        cond = line[5:].strip()  # remove 'while'
        if ' do' in cond:
            cond = cond.split(' do')[0]
        else:
            self.error("while statement must end with 'do'")
            return
        self.emit(f'while ({self.emit_expression(cond)}) {{', indent=self.block_depth)
        self.block_depth += 1

    def compile_for(self, line: str):
        rest = line[3:].strip()  # remove 'for'
        if '=' not in rest:
            self.error("for statement must have an initializer")
            return
        var, rest = rest.split('=', 1)
        var = var.strip()
        if not self.valid_identifier(var):
            self.error(f"invalid for variable '{var}'")
            return
        # parse start, end [, step]
        args = [p.strip() for p in rest.split(',')]
        if len(args) < 2 or len(args) > 3:
            self.error("for expects start, end, and optional step")
            return
        self.add_local(var)
        start = self.emit_expression(args[0])
        end = self.emit_expression(args[1])
        step = self.emit_expression(args[2]) if len(args) == 3 else '1.0'
        self.emit(f'for (double {var} = {start};', indent=self.block_depth)
        self.emit(f'     {var} <= {end};', indent=self.block_depth)
        self.emit(f'     {var} += {step}) {{', indent=self.block_depth)
        self.block_depth += 1

    def compile_local(self, line: str):
        rest = line[5:].strip()  # remove 'local'
        if '=' not in rest:
            self.error("local declaration must initialise a variable")
            return
        var, value = [p.strip() for p in rest.split('=', 1)]
        if not self.valid_identifier(var):
            self.error(f"invalid local variable '{var}'")
            return
        if not self.add_local(var):
            return
        self.emit(f'double {var} = {self.emit_expression(value)};', indent=self.block_depth)

    def compile_return(self, line: str):
        rest = line[6:].strip()
        if rest:
            self.error("return values are not supported by void hooks yet")
            return
        self.emit('return;', indent=self.block_depth)

    def compile_break_continue(self, line: str):
        if self.block_depth <= 0:
            self.error(f"{line} is outside a loop")
            return
        self.emit(f'{line};', indent=self.block_depth)

    def compile_end(self):
        if self.block_depth <= 0:
            self.error("unexpected end")
            return
        self.block_depth -= 1
        self.emit('}', indent=self.block_depth)

    def compile_call(self, line: str):
        # Parse function call: name(args)
        open_paren = line.find('(')
        if open_paren == -1:
            self.error(f"invalid function call '{line}'")
            return
        name = line[:open_paren].strip()
        args_str = line[open_paren+1:-1].strip()
        args = []
        if args_str:
            # simple split by comma (not handling nested, but good enough)
            for a in args_str.split(','):
                a = a.strip()
                if a:
                    args.append(a)

        if name == 'print':
            if len(args) != 1:
                self.error("print expects one argument")
                return
            arg = args[0]
            if arg.startswith('"'):
                self.emit(f'prints({self.emit_expression(arg)});', indent=self.block_depth)
            else:
                self.emit(f'printn({self.emit_expression(arg)});', indent=self.block_depth)
            return

        if name in BUILTIN_FUNCTIONS:
            arg_exprs = ', '.join(self.emit_expression(a) for a in args)
            self.emit(f'{name}({arg_exprs});', indent=self.block_depth)
            return

        if self.function_index(name) >= 0:
            arg_exprs = ', '.join(self.emit_expression(a) for a in args)
            self.emit(f'ds_fn_{name}({arg_exprs});', indent=self.block_depth)
            return

        self.error(f"unknown function '{name}'")

    # ----------------------------------------------------------------------
    # 9. Assignment compilation
    # ----------------------------------------------------------------------

    def find_assignment(self, line: str) -> Optional[Tuple[int, int]]:
        """Find position and length of assignment operator (=, +=, -=, etc.)"""
        depth = 0
        in_string = False
        escaped = False
        for i, ch in enumerate(line):
            if in_string:
                if escaped:
                    escaped = False
                elif ch == '\\':
                    escaped = True
                elif ch == '"':
                    in_string = False
                continue
            if ch == '"':
                in_string = True
            elif ch in '([{':
                depth += 1
            elif ch in ')]}':
                if depth > 0:
                    depth -= 1
            elif depth == 0 and ch in '=+-*/':
                # Skip ==, <=, >=, !=
                if ch == '=' and (i+1 < len(line) and line[i+1] == '='):
                    continue
                if i+1 < len(line) and line[i+1] == '=' and ch in '+-*/':
                    return i, 2  # +=, -=, *=, /=
                if ch == '=':
                    return i, 1
        return None

    def compile_assignment(self, line: str):
        pos = self.find_assignment(line)
        if not pos:
            self.error(f"unsupported statement '{line}'")
            return
        op_pos, op_len = pos
        left = line[:op_pos].strip()
        right = line[op_pos+op_len:].strip()
        if not left or not right:
            self.error("invalid assignment")
            return

        # Parse left side
        object_name = ''
        member = ''
        if '.' in left:
            obj, mem = left.split('.', 1)
            object_name = obj.strip()
            member = mem.strip()
            if not self.valid_identifier(object_name) or not self.valid_identifier(member):
                self.error(f"invalid assignment target '{left}'")
                return
        else:
            object_name = left.strip()
            if not self.valid_identifier(object_name):
                self.error(f"invalid assignment target '{left}'")
                return

        op = '='
        if op_len == 2:
            op = line[op_pos:op_pos+2]  # +=, -=, etc.

        # Handle table literal: { ... }
        if not member and op == '=' and right.startswith('{') and right.endswith('}'):
            self.emit_table_literal(object_name, right)
            return

        # Special case: joy.x = ...
        if member and object_name == 'joy':
            self.emit(f'joy.{member} {op} {self.emit_expression(right)};', indent=self.block_depth)
            return

        # Local variable assignment
        if not member and self.is_local(object_name):
            self.emit(f'{object_name} {op} {self.emit_expression(right)};', indent=self.block_depth)
            return

        # String assignment
        if op == '=' and right.startswith('"'):
            if member:
                self.emit(f'ds_write_string_field("{object_name}", "{member}", {self.emit_expression(right)});',
                          indent=self.block_depth)
            else:
                self.emit(f'ds_write_string("{object_name}", {self.emit_expression(right)});',
                          indent=self.block_depth)
            return

        # General assignment
        if member:
            if op != '=':
                read = self.emit_identifier(object_name, member)
                self.emit(f'ds_write_field("{object_name}", "{member}", {read} {op[:-1]} {self.emit_expression(right)});',
                          indent=self.block_depth)
            else:
                self.emit(f'ds_write_field("{object_name}", "{member}", {self.emit_expression(right)});',
                          indent=self.block_depth)
        else:
            if op != '=':
                read = self.emit_identifier(object_name, None)
                self.emit(f'ds_write("{object_name}", {read} {op[:-1]} {self.emit_expression(right)});',
                          indent=self.block_depth)
            else:
                self.emit(f'ds_write("{object_name}", {self.emit_expression(right)});',
                          indent=self.block_depth)

    def emit_table_literal(self, object_name: str, literal: str):
        """Emit code for table literal: { field1 = val1, field2 = val2 }"""
        content = literal[1:-1].strip()
        fields = []
        if content:
            # simple split by comma (not handling nested tables)
            for item in content.split(','):
                item = item.strip()
                if '=' in item:
                    k, v = item.split('=', 1)
                    fields.append((k.strip(), v.strip()))
                else:
                    self.error(f"table field '{item}' must have a name and value")
                    return

        table_num = self.table_number
        self.table_number += 1
        self.emit(f'{{ Table *ds_table_{table_num} = T_new();', indent=self.block_depth)
        self.emit(f'if (!ds_table_{table_num}) return;', indent=self.block_depth)
        for key, value in fields:
            if not self.valid_identifier(key):
                self.error(f"invalid table field '{key}'")
                continue
            if value.startswith('"'):
                val_expr = f'&(Val){{DS_STRING, {{.str = {self.emit_expression(value)}}}}}'
                type_str = 'DS_STRING'
            elif value.startswith('{'):
                self.error("nested table literals are not supported yet")
                continue
            else:
                val_expr = f'&(Val){{DS_NUMBER, {{.num = {self.emit_expression(value)}}}}}'
                type_str = 'DS_NUMBER'
            self.emit(f'if (!T_set(ds_table_{table_num}, "{key}", {val_expr}, {type_str})) {{ ds_runtime_error("could not assign field \\"{key}\\"); return; }}', indent=self.block_depth)
        self.emit(f'if (!T_set(ds_active_scope(), "{object_name}", ds_table_{table_num}, DS_TABLE)) return;', indent=self.block_depth)
        self.emit('}', indent=self.block_depth)

    # ----------------------------------------------------------------------
    # 10. Top-level generation
    # ----------------------------------------------------------------------

    def line_is_inside_function(self, line_idx: int) -> bool:
        for func in self.functions:
            if func.body_start - 1 <= line_idx <= func.body_end:
                return True
        return False

    def generate_code(self):
        self.output = []

        # Headers
        self.emit('#include "runtime.h"')
        self.emit('#include <math.h>')
        self.emit('#include <string.h>')
        self.emit('')

        # Runtime helpers
        self.emit_runtime_helpers()

        # Function prototypes
        for func in self.functions:
            params = ', '.join(f'double {p}' for p in func.params)
            self.emit(f'static void ds_fn_{func.name}({params});')
        if self.functions:
            self.emit('')

        # Function bodies
        for func in self.functions:
            self.emit_function(func)

        # Hooks
        self.emit_hooks()

    def emit_runtime_helpers(self):
        self.emit('''
#if defined(__GNUC__)
#define DS_UNUSED __attribute__((unused))
#else
#define DS_UNUSED
#endif

static Table *ds_active_scope(void) { return L ? L : G; }
static double ds_read(const char *name) {
    Val *value;
    if (strcmp(name, "screen_w") == 0) return (double)screen_w;
    if (strcmp(name, "screen_h") == 0) return (double)screen_h;
    if (strcmp(name, "fps") == 0) return fps;
    value = T_get(ds_active_scope(), name, NULL);
    if (!value || value->type != DS_NUMBER) {
        ds_runtime_error("'%s' is not a number", name);
        return 0.0;
    }
    return value->num;
}
static double ds_read_field(const char *object, const char *field) {
    Val *object_value = T_get(ds_active_scope(), object, NULL);
    Val *value;
    if (!object_value || object_value->type != DS_TABLE || !object_value->table) {
        ds_runtime_error("'%s' is not a table", object);
        return 0.0;
    }
    value = T_get(object_value->table, field, NULL);
    if (!value || value->type != DS_NUMBER) {
        ds_runtime_error("field '%s.%s' is not a number", object, field);
        return 0.0;
    }
    return value->num;
}
static void ds_write(const char *name, double number) {
    if (!T_set(ds_active_scope(), name, &(Val){DS_NUMBER, {.num = number}}, DS_NUMBER))
        ds_runtime_error("cannot assign '%s'", name);
}
static DS_UNUSED void ds_write_string(const char *name, const char *string) {
    if (!T_set(ds_active_scope(), name, &(Val){DS_STRING, {.str = (char *)string}}, DS_STRING))
        ds_runtime_error("cannot assign '%s'", name);
}
static void ds_write_field(const char *object, const char *field, double number) {
    Val *object_value = T_get(ds_active_scope(), object, NULL);
    if (!object_value || object_value->type != DS_TABLE || !object_value->table) {
        ds_runtime_error("'%s' is not a table", object);
        return;
    }
    if (!T_set(object_value->table, field, &(Val){DS_NUMBER, {.num = number}}, DS_NUMBER))
        ds_runtime_error("cannot assign '%s.%s'", object, field);
}
static DS_UNUSED void ds_write_string_field(const char *object, const char *field, const char *string) {
    Val *object_value = T_get(ds_active_scope(), object, NULL);
    if (!object_value || object_value->type != DS_TABLE || !object_value->table) {
        ds_runtime_error("'%s' is not a table", object);
        return;
    }
    if (!T_set(object_value->table, field, &(Val){DS_STRING, {.str = (char *)string}}, DS_STRING))
        ds_runtime_error("cannot assign '%s.%s'", object, field);
}
''')

    def emit_function(self, func: Function):
        params = ', '.join(f'double {p}' for p in func.params)
        self.emit(f'static void ds_fn_{func.name}({params}) {{')

        self.locals = list(func.params)  # parameters are locals
        self.block_depth = 0
        for i in range(func.body_start, func.body_end):
            line = self.source_lines[i]
            self.set_position(line)
            self.compile_statement_list(line.text)

        if self.block_depth != 0:
            self.error(f"unclosed control block in function '{func.name}'")

        self.emit('}')
        self.emit('')

    def emit_hooks(self):
        def find(name):
            for f in self.functions:
                if f.name == name:
                    return f
            return None

        init_func = find('init')
        update_func = find('update')
        draw_func = find('draw')
        touch_func = find('touch')

        # init
        self.emit('void init(AAssetManager *assets) {')
        self.emit('    (void)assets;')
        self.emit('    T_free(G);')
        self.emit('    G = T_new();')
        self.emit('    L = NULL;')
        self.emit('    if (!G) return;')
        self.locals = []
        self.block_depth = 0
        for i, line in enumerate(self.source_lines):
            if self.line_is_inside_function(i):
                continue
            self.set_position(line)
            self.compile_statement_list(line.text)
        if self.block_depth != 0:
            self.error("unclosed top-level control block")
        if init_func:
            if init_func.params:
                self.error("init() cannot have parameters")
            else:
                self.emit('    ds_fn_init();')
        self.emit('}')
        self.emit('')

        # update
        self.emit('void update(void) {')
        if update_func:
            if update_func.params:
                self.error("update() cannot have parameters")
            else:
                self.emit('    ds_fn_update();')
        self.emit('}')
        self.emit('')

        # draw
        self.emit('void draw(Buffer *buffer) {')
        self.emit('    (void)buffer;')
        if draw_func:
            if draw_func.params:
                self.error("draw() cannot have parameters")
            else:
                self.emit('    ds_fn_draw();')
        self.emit('}')
        self.emit('')

        # touch
        self.emit('void touch(float x, float y, int action) {')
        self.emit('    (void)x; (void)y; (void)action;')
        if touch_func:
            if len(touch_func.params) == 0:
                self.emit('    ds_fn_touch();')
            elif len(touch_func.params) == 3:
                self.emit('    ds_fn_touch((double)x, (double)y, (double)action);')
            else:
                self.error("touch() must have zero or three parameters")
        self.emit('}')
        self.emit('')

    # ----------------------------------------------------------------------
    # 11. Main compile entry
    # ----------------------------------------------------------------------

    def compile(self, source_paths: List[str], output_path: str) -> bool:
        # Load all sources
        for path in source_paths:
            if not self.load_source(path):
                return False

        # Collect functions
        if not self.collect_functions():
            return False

        # Generate code
        self.generate_code()

        # Write output if no errors
        if self.errors == 0:
            try:
                with open(output_path, 'w', encoding='utf-8') as f:
                    f.write('\n'.join(self.output))
                return True
            except Exception as e:
                print(f"Error: cannot write {output_path}: {e}", file=sys.stderr)
                return False
        else:
            return False


# ----------------------------------------------------------------------
# 12. Command-line interface
# ----------------------------------------------------------------------

def main():
    if len(sys.argv) == 1:
        print("Usage: python ds_compiler.py [--output out.c] file1.ds [file2.ds ...]", file=sys.stderr)
        sys.exit(2)

    output = None
    sources = []
    i = 1
    while i < len(sys.argv):
        if sys.argv[i] == '--output':
            if i+1 < len(sys.argv):
                output = sys.argv[i+1]
                i += 2
            else:
                print("Error: --output requires a filename", file=sys.stderr)
                sys.exit(2)
        else:
            sources.append(sys.argv[i])
            i += 1

    if not sources:
        print("Error: no input files", file=sys.stderr)
        sys.exit(2)

    if not output:
        # If only one source, use basename .c
        if len(sources) == 1:
            output = os.path.splitext(sources[0])[0] + '.c'
        else:
            output = 'game.c'

    comp = Compiler()
    success = comp.compile(sources, output)
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
