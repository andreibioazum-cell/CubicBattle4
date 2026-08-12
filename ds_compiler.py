#!/usr/bin/env python3
"""DimScript — минимальный язык для игры, компиляция в C.

Типы: num, str, col. Блоки закрываются словом `end`, вызовы — без скобок.

    str TEX = "player.png"

    object Player              // структура с полями
        num x = 0
        col color = 0xFF8844
    end

    Player player = new Player()
    player.x = 100

    function move_player num dx, num dy
        player.x = player.x + dx
        if player.x > 10
            return
        end
        loop player.x < 100
            player.x = player.x + 1
        end
    end

    move_player 10, 0          // вызов функции
    circle player.x, player.y, 15, player.color   // встроенная функция
"""

import os
import re
import sys

# Типы языка и их C-представление.
TYPES = {
    'num': 'double',
    'str': 'const char *',
    'col': 'uint32_t',
    'bool': 'int',
    'arr': 'DSArray*',
    'dict': 'DSDict*',
    'timer': 'DSTimer*',
}

# Встроенные функции (имя в скрипте = имя в C).
BUILTINS = frozenset({
    # отрисовка
    'rect', 'roundrect', 'circle', 'ring', 'line', 'tex', 'text',
    'text_scaled', 'text_ink_width', 'text_ink_height', 'text_ink_top', 'png_load',
    # математика
    'sqrt', 'sin', 'cos', 'atan2', 'floor', 'rand',
    # звёзды (совместимость)
    # онлайн Firebase
    'net_connect', 'net_disconnect', 'net_publish', 'net_publish_bullet',
    'net_status', 'net_slot', 'net_count',
    'net_player_online', 'net_player_x', 'net_player_y', 'net_player_angle',
    'net_player_hp', 'net_player_alive',
    'net_player_bullet_active', 'net_player_bullet_x', 'net_player_bullet_y',
    'net_player_bullet_dx', 'net_player_bullet_dy', 'net_player_bullet_shot',
    'net_player_bullet_tr',
    # лог
    'ds_log',
    # --- новые типы: массивы
    'arr_new', 'arr_push', 'arr_pop', 'arr_get', 'arr_set', 'arr_len', 'arr_clear', 'arr_free',
    # словари
    'dict_new', 'dict_set', 'dict_get', 'dict_has', 'dict_del', 'dict_free',
    # таймеры
    'timer_new', 'timer_start', 'timer_elapsed', 'timer_reset', 'timer_free',
    # файлы
    'file_read', 'file_write', 'file_exists', 'file_del',
    # json
    'json_get_str', 'json_get_num', 'json_get_bool',
    # сеть высокого уровня
    'http_get', 'http_post',
    # утилиты
    'clamp', 'lerp', 'dist', 'now',
})

# Глобальные переменные хоста (read-only, кроме joy).
ENGINE_VARS = {'screen_w': 'num', 'screen_h': 'num', 'dt': 'num', 'joy': 'joy'}

_NAME = r'[A-Za-z_]\w*'
_DECL_RE = re.compile(r'^(' + _NAME + r')\s+(' + _NAME + r')\s*(?:=\s*(.*))?$')
_FUNC_RE = re.compile(r'^function\s+(' + _NAME + r')(?:\s+(.*))?$')
_NUM_RE = re.compile(r'^(?:[-+]?\d+(?:\.\d+)?|0[xX][0-9a-fA-F]+)$')
_CALL_RE = re.compile(r'^(' + _NAME + r')(?:\s+(.*))?$')
_LHS_RE = re.compile(r'^(' + _NAME + r')(?:\.(' + _NAME + r'))?$')


def strip_comment(line):
    """Убирает //-комментарий, не трогая строковые литералы."""
    out = []
    i = 0
    in_str = False
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif c == '/' and i + 1 < len(line) and line[i + 1] == '/':
            break
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def scan(text):
    """Для каждого символа: вложенность скобок и флаг «внутри строки»."""
    n = len(text)
    depth = [0] * n
    quoted = [False] * n
    in_str = False
    escaped = False
    level = 0
    for i, c in enumerate(text):
        quoted[i] = in_str
        if escaped:
            escaped = False
            continue
        if in_str:
            if c == '\\':
                escaped = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == '(':
            level += 1
        elif c == ')':
            level = max(0, level - 1)
        depth[i] = level
    return depth, quoted


def split_top(text, sep):
    """Делит выражение по разделителю на верхнем уровне (вне строк и скобок)."""
    depth, quoted = scan(text)
    parts = []
    start = 0
    for i, c in enumerate(text):
        if depth[i] == 0 and not quoted[i] and c == sep:
            parts.append(text[start:i].strip())
            start = i + 1
    parts.append(text[start:].strip())
    return parts


def find_assign(line):
    """Индекс простого `=` (не ==, !=, <=, >=) вне строк и скобок, иначе -1."""
    depth, quoted = scan(line)
    for i, c in enumerate(line):
        if quoted[i] or depth[i]:
            continue
        if c == '=' and (i == 0 or line[i - 1] not in '<>!') and (
                i + 1 >= len(line) or line[i + 1] != '='):
            return i
    return -1


def used_outside_strings(text, name):
    """Встречается ли идентификатор в тексте вне строковых литералов."""
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    _, quoted = scan(text)
    for m in pat.finditer(text):
        if not quoted[m.start()]:
            return True
    return False


class DimScriptCompiler:
    def __init__(self):
        self.objects = {}        # имя -> поля {имя: (тип, значение)}
        self.vars = {}           # глобальные переменные {имя: (тип, значение)}
        self.functions = {}      # имя -> (параметры [(тип, имя)], тело [строки])
        self.func_ret = {}       # имя -> 'num', если функция возвращает значение
        self.top = []            # исполняемые строки верхнего уровня
        self.lines = []          # все строки исходника после include-склейки
        self.loaded_sources = [] # прочитанные файлы
        self.errors = 0
        # состояние генерации
        self.output = []
        self.indent = 0
        self.scope = {}          # локальные переменные и параметры: имя -> тип
        self.blocks = []         # стек открытых if/loop

    # ---------- чтение и разбор ----------

    def _error(self, msg):
        self.errors += 1
        print(f"DimScript error: {msg}", file=sys.stderr)

    def _load(self, paths):
        for path in paths:
            try:
                with open(path, 'r', encoding='utf-8-sig') as f:
                    for raw in f:
                        line = strip_comment(raw).strip()
                        if not line:
                            continue
                        # ; как разделитель операторов — уменьшает строки
                        for part in split_top(line, ';'):
                            p = part.strip()
                            if p:
                                self.lines.append(p)
                self.loaded_sources.append(os.path.abspath(path))
            except OSError as e:
                self._error(f"cannot read '{path}': {e}")
                return False
        return True
    def _decl_list(self, line):
        # поддерживает: num x=0, y=1, z
        m = re.match(r'^(' + _NAME + r')\s+(.+)$', line)
        if not m:
            return None
        t, rest = m.group(1), m.group(2).strip()
        if t not in TYPES and t not in self.objects and t != 'joy':
            # тип может быть неизвестен на раннем этапе — проверим хотя бы синтаксис
            if not re.match(r'^[A-Za-z_]', t):
                return None
        # разбить по , на верхнем уровне
        parts = split_top(rest, ',')
        res = []
        for part in parts:
            part = part.strip()
            if not part:
                continue
            mm = re.match(r'^(' + _NAME + r')(?:\s*=\s*(.*))?$', part)
            if not mm:
                return None
            n, v = mm.group(1), mm.group(2)
            if v:
                v = v.strip()
            res.append((t, n, v))
        return res if res else None

    def parse(self):
        i = 0
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                self._error("unexpected 'end' at top level")
                i += 1
            elif line.startswith('object '):
                i = self._parse_object(i)
            elif line.startswith('enum '):
                i = self._parse_enum(i)
            elif line.startswith('function '):
                i = self._parse_function(i)
            elif self._decl_all(line):
                self._parse_global(line)
                i += 1
            else:
                self.top.append(line)
                i += 1
        return self.errors == 0
    def _parse_enum(self, i):
        # enum Name  A=0, B, C=5  end  -> создает глобальные const num
        m = re.match(r'^enum\s+(' + _NAME + r')(?:\s+(.*))?$', self.lines[i])
        if not m:
            self._error(f"invalid enum: {self.lines[i]}")
            return i+1
        ename = m.group(1)
        rest = (m.group(2) or "").strip()
        members = []
        if rest:
            # разрешаем члены на той же строке через , 
            for p in split_top(rest, ','):
                p=p.strip()
                if p and p!='end':
                    members.append(p)
        j = i+1
        while j < len(self.lines):
            line = self.lines[j]
            if line == 'end':
                break
            # каждая строка может содержать несколько членов через ,
            for p in split_top(line, ','):
                p=p.strip()
                if p:
                    members.append(p)
            j+=1
        # теперь разобрать members
        cur = 0
        for mem in members:
            if not mem:
                continue
            mm = re.match(r'^(' + _NAME + r')(?:\s*=\s*(.*))?$', mem)
            if not mm:
                self._error(f"enum {ename}: bad member '{mem}'")
                continue
            n, v = mm.group(1), mm.group(2)
            if v is not None:
                v=v.strip()
                try:
                    cur = int(float(v)) if '.' in v else int(v)
                except:
                    cur = 0
                self.vars[n] = ('num', str(cur))
            else:
                self.vars[n] = ('num', str(cur))
            cur+=1
        return j+1

    def _decl(self, line):
        lst = self._decl_list(line)
        if not lst:
            return None
        # для совместимости возвращаем первый
        t, n, v = lst[0]
        if t not in TYPES and t not in self.objects:
            return None
        return t, n, v.strip() if v else None
    def _decl_all(self, line):
        # возвращает все объявления в строке: num a=0, b=1
        lst = self._decl_list(line)
        if not lst:
            return None
        # проверим типы
        for t, n, v in lst:
            if t not in TYPES and t not in self.objects:
                # на этапе парсинга объектов тип может быть ещё не известен? проверим только буквы
                if t not in TYPES and t not in self.objects:
                    # если это поле объекта, тип должен быть из TYPES
                    if t not in TYPES:
                        return None
        return lst

    def _parse_object(self, i):
        # object Name  (num x=0, y=0  ...) end — поля могут быть и на той же строке.
        m = re.match(r'^object\s+(' + _NAME + r')(?:\s+(.+))?$', self.lines[i])
        if not m:
            self._error(f"invalid object declaration: {self.lines[i]}")
            return i + 1
        name, rest = m.group(1), (m.group(2) or '').strip()
        if name in self.objects:
            self._error(f"duplicate object '{name}'")
            return i + 1
        fields = {}
        if rest and rest != 'end':
            lst = self._decl_list(rest)
            if lst:
                for t, n, v in lst:
                    if t not in TYPES:
                        self._error(f"object '{name}': expected type, got: {rest}")
                    else:
                        fields[n] = (t, v)
        j = i + 1
        while j < len(self.lines):
            line = self.lines[j]
            if line == 'end':
                self.objects[name] = fields
                return j + 1
            lst = self._decl_all(line)
            if not lst:
                self._error(f"object '{name}': expected 'type name = value', got: {line}")
            else:
                for t, n, v in lst:
                    if t not in TYPES:
                        self._error(f"object '{name}': expected type, got: {line}")
                    elif n in fields:
                        self._error(f"duplicate field '{name}.{n}'")
                    else:
                        fields[n] = (t, v)
            j += 1
        self._error(f"object '{name}' has no closing 'end'")
        return j

    def _parse_function(self, i):
        m = _FUNC_RE.match(self.lines[i])
        name = m.group(1)
        params = self._parse_params(m.group(2) or '')
        body, j = self._collect_block(i + 1, f"function '{name}'")
        if name in self.functions:
            self._error(f"duplicate function '{name}'")
        else:
            self.functions[name] = (params, body)
            if any(line.startswith('return ') for line in body):
                self.func_ret[name] = 'num'   # уточняется в _infer_returns()
        return j

    def _infer_returns(self):
        """Уточняет тип возвращаемого значения: num или str.

        Литерал "..." или str-выражение делают функцию строковой. Проходим
        несколько раз, чтобы функция, возвращающая результат другой функции,
        тоже получила верный тип."""
        for _pass in range(4):
            changed = False
            for name in self.func_ret:
                params, body = self.functions[name]
                saved = self.scope
                self.scope = {pn: pt for pt, pn in params}
                kind = 'num'
                for line in body:
                    if line.startswith('return '):
                        if self.expr_type(line[7:].strip()) == 'str':
                            kind = 'str'
                            break
                self.scope = saved
                if self.func_ret[name] != kind:
                    self.func_ret[name] = kind
                    changed = True
            if not changed:
                break

    def _parse_params(self, text):
        params = []
        if not text.strip():
            return params
        for part in split_top(text, ','):
            w = part.split()
            if len(w) != 2 or (w[0] not in TYPES and w[0] not in self.objects):
                self._error(f"invalid parameter '{part}'; expected 'type name'")
                continue
            params.append((w[0], w[1]))
        return params

    def _collect_block(self, i, what):
        """Собирает тело блока (function/object) до парного `end`."""
        depth = 0
        body = []
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                if depth == 0:
                    return body, i + 1
                depth -= 1
            elif line.startswith('if ') or line.startswith('loop '):
                depth += 1
            elif line == 'else' or line.startswith('else if '):
                if depth == 0:
                    self._error(f"{what}: 'else' without 'if'")
                    return body, i + 1
            elif line.startswith('object ') or line.startswith('function '):
                self._error(f"{what}: nested 'object'/'function' is not allowed")
                body.append(line)
                i += 1
                continue
            body.append(line)
            i += 1
        self._error(f"{what} has no closing 'end'")
        return body, i

    def _parse_global(self, line):
        lst = self._decl_all(line)
        if not lst:
            return
        for t, n, v in lst:
            if n in self.vars:
                self._error(f"duplicate variable '{n}'")
                continue
            if t in self.objects:
                if not v or not re.match(r'^new\s+' + re.escape(t) + r'\s*\(\)?\s*$', v):
                    self._error(f"'{n}': object variable must be 'new {t}()'")
                    continue
            self.vars[n] = (t, v)

    # ---------- типы и выражения ----------

    def c_type(self, t):
        if t in TYPES:
            return TYPES[t]
        if t in self.objects:
            return t + ' *'
        return t

    def default_value(self, t):
        return 'NULL' if t == 'str' else '0'

    def static_expr(self, v):
        return bool(_NUM_RE.match(v)) or (
            len(v) >= 2 and v[0] == '"' and v[-1] == '"')

    def expr_type(self, expr):
        expr = expr.strip()
        if expr in ('true', 'false'):
            return 'bool'
        if expr.startswith('"') and expr.endswith('"'):
            return 'str'
        m = re.match(r'^(' + _NAME + r')\.(' + _NAME + r')$', expr)
        if m:
            holder = m.group(1)
            ot = self.scope.get(holder) or (
                self.vars[holder][0] if holder in self.vars else None)
            fields = self.objects.get(ot)
            if fields and m.group(2) in fields:
                return fields[m.group(2)][0]
        if expr in self.scope:
            return self.scope[expr]
        if expr in self.vars:
            return self.vars[expr][0]
        call = re.match(r'^(' + _NAME + r')\s*\(.*\)$', expr)
        if call and call.group(1) in self.func_ret:
            return self.func_ret[call.group(1)]
        return ENGINE_VARS.get(expr, 'num')

    def expr(self, e):
        """Переводит выражение в C."""
        e = e.strip()
        if e == 'true':
            return '1'
        if e == 'false':
            return '0'
        parts = split_top(e, '+')
        if len(parts) > 1 and all(parts) and any(
                self.expr_type(p) == 'str' for p in parts):
            out = self.as_str(parts[0])
            for p in parts[1:]:
                out = f'ds_concat({out}, {self.as_str(p)})'
            return out
        return self._fields(e)

    def _fields(self, e):
        """Превращает obj.field в obj->field вне строковых литералов.
        Работает и для глобальных объектов, и для параметров-объектов."""
        names = [n for n in self.vars if self.vars[n][0] in self.objects]
        names += [n for n, t in self.scope.items() if t in self.objects]
        for n in sorted(names, key=len, reverse=True):
            pat = re.compile(r'\b' + re.escape(n) + r'\.(' + _NAME + r')')
            repl = n + r'->\1'
            _, quoted = scan(e)
            if not any(quoted):
                e = pat.sub(repl, e)
                continue
            out = []
            start = 0
            for m in pat.finditer(e):
                if quoted[m.start()]:
                    continue
                out.append(e[start:m.start()])
                out.append(m.expand(repl))
                start = m.end()
            out.append(e[start:])
            e = ''.join(out)
        return self._calls(e)

    def _calls(self, e):
        """Имя(...) пользовательской функции -> ds_fn_имя(...)."""
        if not self.functions:
            return e
        _, quoted = scan(e)
        pattern = re.compile(r'\b(' + _NAME + r')\s*\(')
        out = []
        start = 0
        for m in pattern.finditer(e):
            name = m.group(1)
            if quoted[m.start()] or name not in self.functions:
                continue
            out.append(e[start:m.start()])
            out.append('ds_fn_' + name + '(')
            start = m.end()
        out.append(e[start:])
        return ''.join(out)

    def as_str(self, e):
        if self.expr_type(e) == 'str':
            return self.expr(e)
        return f'ds_num_to_string((double)({self.expr(e)}))'

    # ---------- генерация C ----------

    def _out(self, s):
        self.output.append('    ' * self.indent + s)

    def _emit(self, s):
        self.output.append(s)

    def _emit_line(self, line):
        # inline C: c <raw C>  или  c "C code"  — вызов кода на Си из DimScript
        if line and line[0] == 'c' and len(line) > 1 and line[1] in ' \t"':
            if find_assign(line) == -1 and not self._decl(line):
                raw = line[1:].strip()
                if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
                    inner = raw[1:-1]
                    inner = inner.replace('\\"', '"').replace('\\\\', '\\')
                    self._out(inner)
                else:
                    if raw:
                        # `;` is consumed as a statement separator in _load, so
                        # restore the trailing semicolon for C statements that
                        # are not already terminated by ; { or }.
                        if raw.endswith((';', '{', '}')):
                            self._out(raw)
                        else:
                            self._out(raw + ';')
                return
        if line == 'end':
            if not self.blocks:
                self._error("unexpected 'end'")
                return
            self.blocks.pop()
            self.indent -= 1
            self._out('}')
            return
        if line.startswith('if '):
            cond = line[3:].strip()
            # поддержка then в конце
            if cond.endswith(' then'):
                cond = cond[:-5].strip()
            elif cond.endswith(' then:'):
                cond = cond[:-6].strip()
            if cond.endswith(':'):
                cond = cond[:-1].strip()
            self._open_block(f'if ({self.expr(cond)})')
            return
        if line.startswith('loop '):
            cond = line[5:].strip()
            if cond.endswith(' do'):
                cond = cond[:-3].strip()
            elif cond.endswith(' do:'):
                cond = cond[:-4].strip()
            if cond.endswith(':'):
                cond = cond[:-1].strip()
            self._open_block(f'while ({self.expr(cond)})')
            return
        if line == 'else' or line.startswith('else if '):
            if not self.blocks:
                self._error("'else' without 'if'")
                return
            header = 'else' if line == 'else' else f'else if ({self.expr(line[8:])})'
            self.indent -= 1
            self._out(f'}} {header} {{')
            self.indent += 1
            return
        if line == 'return':
            self._out('return;')
            return
        if line.startswith('return '):
            self._out(f'return {self.expr(line[7:])};')
            return
        lst = self._decl_all(line)
        if lst and lst[0][0] in TYPES:
            for t, n, v in lst:
                if n in self.scope:
                    self._error(f"duplicate variable '{n}'")
                    continue
                self.scope[n] = t
                init = f'= {self.expr(v)}' if v else f'= {self.default_value(t)}'
                self._out(f'{self.c_type(t)} {n} {init};')
            return
        self._emit_statement(line)

    def _open_block(self, header):
        self.blocks.append(header)
        self._out(header + ' {')
        self.indent += 1

    def _emit_statement(self, line):
        i = find_assign(line)
        if i >= 0:
            self._emit_assign(line[:i].strip(), line[i + 1:].strip())
            return
        m = _CALL_RE.match(line)
        if not m:
            self._error(f"invalid statement: {line}")
            return
        name, rest = m.group(1), m.group(2) or ''
        args = split_top(rest, ',') if rest else []
        if name in self.functions:
            if len(args) != len(self.functions[name][0]):
                self._error(f"function '{name}' expects "
                            f"{len(self.functions[name][0])} argument(s), got {len(args)}")
                return
            fn = f'ds_fn_{name}'
        elif name in BUILTINS:
            fn = name
        elif name in ENGINE_VARS or name in self.vars or name in self.scope:
            self._error(f"'{name}' is a variable, not a function")
            return
        else:
            self._error(f"unknown function '{name}'")
            return
        args_c = ', '.join(self.expr(a) for a in args)
        self._out(f'{fn}({args_c});')

    def _emit_assign(self, lhs, rhs):
        m = _LHS_RE.match(lhs)
        if not m:
            self._error(f"invalid assignment target: {lhs}")
            return
        name, field = m.group(1), m.group(2)
        if rhs.startswith('new '):
            self._error(f"'{lhs} = {rhs}': use 'Type name = new Type()'")
            return
        if field:
            t = self.scope.get(name) or self.vars.get(name, ('', None))[0]
            if t not in self.objects and ENGINE_VARS.get(name) != 'joy':
                self._error(f"unknown object '{name}'")
                return
        elif name not in self.scope and name not in self.vars and name not in ENGINE_VARS:
            self._error(f"unknown variable '{name}'")
        holder_type = self.scope.get(name) or self.vars.get(name, ('', None))[0]
        if field and holder_type in self.objects:
            lhs = self._fields(lhs)
        self._out(f'{lhs} = {self.expr(rhs)};')

    def generate(self):
        self._infer_returns()
        self.output = []
        self.indent = 0
        self._emit('#include "runtime.h"')
        self._emit('#include "net.h"')
        self._emit('#include <math.h>')
        self._emit('')
        # структуры объектов
        for name in self.objects:
            self._emit(f'typedef struct {name} {name};')
        self._emit('')
        for name, fields in self.objects.items():
            self._emit(f'struct {name} {{')
            for f, (t, _v) in fields.items():
                self._emit(f'    {self.c_type(t)} {f};')
            self._emit('};')
        self._emit('')
        for name in self.objects:
            self._emit(f'static {name} *ds_new_{name}(void);')
            self._emit(f'static void ds_free_{name}({name} *self);')
        self._emit('')
        # глобальные переменные
        init_lines = []
        for n, (t, v) in self.vars.items():
            if t in self.objects:
                self._emit(f'{t} *{n} = NULL;')
                init_lines.append(n)
            elif v and self.static_expr(v):
                self._emit(f'{self.c_type(t)} {n} = {self.expr(v)};')
            else:
                self._emit(f'{self.c_type(t)} {n} = {self.default_value(t)};')
                if v:
                    init_lines.append(n)
        if self.vars:
            self._emit('')
        # прототипы функций
        for n, (params, _b) in self.functions.items():
            self._emit(f'static {self._ret_c(n)} ds_fn_{n}({self._params_c(params)});')
        if self.functions:
            self._emit('')
        # конструкторы и деструкторы
        for name, fields in self.objects.items():
            self._emit(f'static {name} *ds_new_{name}(void) {{')
            self._emit(f'    {name} *self = ({name} *)calloc(1, sizeof(*self));')
            self._emit(f'    if (!self) {{ ds_runtime_error("out of memory: {name}"); return NULL; }}')
            for f, (t, v) in fields.items():
                if v:
                    self._emit(f'    self->{f} = {self.expr(v)};')
            self._emit('    return self;')
            self._emit('}')
            self._emit(f'static void ds_free_{name}({name} *self) {{ free(self); }}')
            self._emit('')
        # функции
        for n, (params, body) in self.functions.items():
            self._emit(f'static {self._ret_c(n)} ds_fn_{n}({self._params_c(params)}) {{')
            self.indent = 1
            self.scope = {pn: pt for pt, pn in params}
            self.blocks = []
            body_text = '\n'.join(body)
            for _pt, pn in params:
                if not used_outside_strings(body_text, pn):
                    self._out(f'(void){pn};')
            for line in body:
                self._emit_line(line)
            if self.blocks:
                self._error(f"function '{n}': missing 'end'")
                self.blocks = []
            self._emit('}')
            self._emit('')
        # запуск верхнего уровня (объекты и команды в порядке исходника)
        self._emit('static int ds_main(void) {')
        self.indent = 1
        self.scope = {}
        self.blocks = []
        for n in init_lines:
            t = self.vars[n][0]
            if t in self.objects:
                self._out(f'{n} = ds_new_{t}();')
            else:
                self._out(f'{n} = {self.expr(self.vars[n][1])};')
        for line in self.top:
            self._emit_line(line)
        if self.blocks:
            self._error("top level: missing 'end'")
            self.blocks = []
        self._emit('    return 0;')
        self._emit('}')
        self._emit('')
        # хуки хоста
        self._emit('void reset(void) {')
        self.indent = 1
        for n, (t, v) in self.vars.items():
            if t in self.objects:
                self._out(f'if ({n}) ds_free_{t}({n});')
                self._out(f'{n} = NULL;')
            elif t == 'arr':
                self._out(f'if ({n}) arr_free({n});')
                self._out(f'{n} = arr_new();')
            elif t == 'dict':
                self._out(f'if ({n}) dict_free({n});')
                self._out(f'{n} = dict_new();')
            elif v and self.static_expr(v):
                self._out(f'{n} = {self.expr(v)};')
            else:
                self._out(f'{n} = {self.default_value(t)};')
        self._emit('}')
        self._emit('')
        self._emit('void init(AAssetManager *assets) {')
        self.indent = 1
        self._out('ds_set_asset_manager(assets);')
        self._out('ds_main();')
        if 'init' in self.functions:
            self._out('ds_fn_init();')
        self._emit('}')
        self._emit('')
        self._emit('void update(void) {')
        self.indent = 1
        if 'update' in self.functions:
            self._out('ds_fn_update();')
        self._emit('}')
        self._emit('')
        self._emit('void draw(Buffer *buffer) {')
        self.indent = 1
        self._out('(void)buffer;')
        if 'draw' in self.functions:
            self._out('ds_fn_draw();')
        self._emit('}')
        self._emit('')
        self._emit('void touch(float x, float y, int action, int pointer_id) {')
        self.indent = 1
        if 'touch' in self.functions:
            args = []
            for i, (pt, _pn) in enumerate(self.functions['touch'][0]):
                if i >= 4:
                    break
                if pt == 'str':
                    self._error("'touch' parameters cannot be 'str'")
                    break
                args.append(f'({self.c_type(pt)}){("x", "y", "action", "pointer_id")[i]}')
            self._out(f'ds_fn_touch({", ".join(args)});')
        else:
            self._out('(void)x; (void)y; (void)action; (void)pointer_id;')
        self._emit('}')

    def _ret_c(self, name):
        return self.c_type(self.func_ret[name]) if name in self.func_ret else 'void'

    def _params_c(self, params):
        if not params:
            return 'void'
        return ', '.join(f'{self.c_type(t)} {n}' for t, n in params)

    def compile(self, sources, output):
        if not self._load(sources):
            return False
        if not self.parse():
            return False
        self.generate()
        if self.errors:
            return False
        with open(output, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.output) + '\n')
        return True


def main():
    output = 'game/game.c'
    sources = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ('-o', '--output') and i + 1 < len(args):
            output = args[i + 1]
            i += 2
        else:
            sources.append(args[i])
            i += 1
    if not sources:
        print("Usage: python ds_compiler.py file.ds [-o output.c]", file=sys.stderr)
        sys.exit(2)
    ok = DimScriptCompiler().compile(sources, output)
    print(f"{output}: {'OK' if ok else 'FAILED'}")
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
