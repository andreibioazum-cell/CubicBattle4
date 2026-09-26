"""Code generation: return inference and the whole game.c layout."""

import re

from .lexer import _NAME, used_outside_strings


class CodegenMixin:
    """Code generation: return inference and the whole game.c layout."""

    def _infer_returns(self):
        keys = [n for n in self.functions]
        keys += [f'{c}.{m}' for c, ms in self.methods.items() for m in ms]
        for _ in range(4):
            changed = False
            for key in keys:
                if key in self.functions:
                    _vis, params, body = self.functions[key]
                else:
                    c, m = key.split('.')
                    _v, _s, params, _r, body = self.methods[c][m]
                if self.func_ret.get(key) not in (None, 'num'):
                    continue
                saved_scope, saved_class = self.scope, self.cur_class
                self.scope = {pn: pt for pt, pn in params}
                if '.' in key:
                    self.cur_class = key.split('.')[0]
                    self.scope['self'] = self.cur_class
                kind = 'num'
                for line in body:
                    lst = self._decl_list(line)
                    if lst and lst[0][1]:
                        for _v, _l, t, n, v in lst:
                            self.scope[n] = t
                        continue
                    lm = re.match(r'^local\s+(' + _NAME + r')\s*=\s*(.+)$', line)
                    if lm:
                        t = self.expr_type(lm.group(2))
                        self.scope[lm.group(1)] = t if t else 'num'
                        continue
                    if line.startswith('return '):
                        if self.expr_type(line[7:].strip()) == 'str':
                            kind = 'str'
                            break
                self.scope, self.cur_class = saved_scope, saved_class
                if self.func_ret.get(key) != kind:
                    self.func_ret[key] = kind
                    changed = True
            if not changed:
                break

    def _ret_c(self, key):
        r = self.func_ret.get(key)
        if not r or r == 'void':
            return 'void'
        return self.c_type(r)

    def _params_c(self, params):
        if not params:
            return 'void'
        return ', '.join(f'{self.c_type(t)} {n}' for t, n in params)

    def generate(self):
        self._infer_returns()
        self.output = []
        self.indent = 0
        self._emit('#include "runtime.h"')
        self._emit('#include "net.h"')
        self._emit('#include <math.h>')
        self._emit('')
        for name in self.objects:
            self._emit(f'typedef struct {name} {name};')
        self._emit('')
        for name, fields in self.objects.items():
            self._emit(f'struct {name} {{')
            for f, (_vis, t, _v) in fields.items():
                self._emit(f'    {self.c_type(t)} {f};')
            self._emit('};')
        self._emit('')
        for name in self.objects:
            self._emit(f'static {name} *ds_new_{name}(void);')
            self._emit(f'static void ds_free_{name}({name} *self);')
        for cls, ms in self.methods.items():
            for mname, (vis, static, params, _ret, _body) in ms.items():
                if mname == 'new':
                    args, pre = f'{cls} *self', 'ds_mth_'
                elif static:
                    args, pre = self._params_c(params), 'ds_stc_'
                else:
                    args = f'{cls} *self' + (', ' + self._params_c(params) if params else '')
                    pre = 'ds_mth_'
                self._emit(f'static {self._ret_c(cls + "." + mname)} {pre}{cls}_{mname}({args});')
        self._emit('')
        init_lines = []
        for n, (_vis, t, v) in self.vars.items():
            if t in self.objects:
                self._emit(f'{t} *{n} = NULL;')
            elif v and self.static_expr(v):
                self._emit(f'{self.c_type(t)} {n} = {self.expr(v)};')
            else:
                self._emit(f'{self.c_type(t)} {n} = {self.default_value(t)};')
            if v:
                init_lines.append(n)
        if self.vars:
            self._emit('')
        for n, (_vis, _params, _b) in self.functions.items():
            self._emit(f'static {self._ret_c(n)} ds_fn_{n}({self._params_c(_params)});')
        if self.functions:
            self._emit('')
        for name, fields in self.objects.items():
            self._emit(f'static {name} *ds_new_{name}(void) {{')
            self._emit(f'    {name} *self = ({name} *)calloc(1, sizeof(*self));')
            self._emit(f'    if (!self) {{ ds_runtime_error("out of memory: {name}"); return NULL; }}')
            for f, (_vis, _t, v) in fields.items():
                if v:
                    self._emit(f'    self->{f} = {self.expr(v)};')
            ctor = self.methods.get(name, {}).get('new')
            if ctor:
                self._emit(f'    ds_mth_{name}_new(self);')
            self._emit('    return self;')
            self._emit('}')
            self._emit(f'static void ds_free_{name}({name} *self) {{ free(self); }}')
            self._emit('')
        for cls, ms in self.methods.items():
            for mname, (vis, static, params, _ret, body) in ms.items():
                if mname == 'new':
                    cname = f'ds_mth_{cls}_new'
                    args = f'{cls} *self'
                elif static:
                    cname = f'ds_stc_{cls}_{mname}'
                    args = self._params_c(params)
                else:
                    cname = f'ds_mth_{cls}_{mname}'
                    args = f'{cls} *self' + (', ' + self._params_c(params) if params else '')
                self._emit(f'static {self._ret_c(cls + "." + mname)} {cname}({args}) {{')
                self.indent = 1
                self.scope = {pn: pt for pt, pn in params}
                if not static:
                    self.scope['self'] = cls
                self.cur_class = cls
                self.blocks = []
                body_text = '\n'.join(body)
                for _pt, pn in params:
                    if not used_outside_strings(body_text, pn):
                        self._out(f'(void){pn};')
                if mname == 'new' and not used_outside_strings(body_text, 'self'):
                    self._out('(void)self;')
                for line in body:
                    self._emit_line(line, f'{cls}.{mname}')
                self._emit('}')
                self._emit('')
        for n, (_vis, params, body) in self.functions.items():
            self._emit(f'static {self._ret_c(n)} ds_fn_{n}({self._params_c(params)}) {{')
            self.indent = 1
            self.scope = {pn: pt for pt, pn in params}
            self.cur_class = None
            self.blocks = []
            body_text = '\n'.join(body)
            for _pt, pn in params:
                if not used_outside_strings(body_text, pn):
                    self._out(f'(void){pn};')
            for line in body:
                self._emit_line(line, n)
            self._emit('}')
            self._emit('')
        self._emit('static int ds_main(void) {')
        self.indent = 1
        self.scope = {}
        self.cur_class = None
        self.blocks = []
        for n in init_lines:
            t = self.vars[n][1]
            if t in self.objects:
                self._out(f'{n} = ds_new_{t}();')
            elif self.vars[n][2]:
                try:
                    self._out(f'{n} = {self.expr(self.vars[n][2])};')
                except Exception:
                    pass
        for line in self.top:
            self._emit_line(line, 'top level')
        self._emit('    return 0;')
        self._emit('}')
        self._emit('')
        self._emit('void reset(void) {')
        self.indent = 1
        for n, (_vis, t, v) in self.vars.items():
            if t in self.objects:
                self._out(f'if ({n}) ds_free_{t}({n});')
                self._out(f'{n} = NULL;')
            elif t == 'arr':
                self._out(f'if ({n}) arr_free({n});')
                self._out(f'{n} = arr_new();')
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
        self._emit('int back_pressed(void) {')
        self.indent = 1
        if 'back_pressed' in self.functions and self.func_ret.get('back_pressed') == 'num':
            self._out('return ds_fn_back_pressed() != 0;')
        else:
            self._out('return 0;')
        self._emit('}')
        self._emit('')
        self._emit('void touch(float x, float y, int action, int pointer_id) {')
        self.indent = 1
        self._out('mouse_clicked = (action == 0) ? 1 : 0;')
        self._out('if (action == 0) { ds_mouse_x = x; ds_mouse_y = y; }')
        if 'touch' in self.functions:
            args = []
            for i, (pt, _pn) in enumerate(self.functions['touch'][1]):
                if i >= 4 or pt == 'str':
                    break
                args.append(f'({self.c_type(pt)}){("x", "y", "action", "pointer_id")[i]}')
            self._out(f'ds_fn_touch({", ".join(args)});')
        else:
            self._out('(void)x; (void)y; (void)action; (void)pointer_id;')
        self._emit('}')
