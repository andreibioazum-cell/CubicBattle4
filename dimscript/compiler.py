"""DimScriptCompiler: compiler state plus the loading, parsing, typing,
expression, statement and generation stages, one mixin per module."""

import sys

from .codegen import CodegenMixin
from .emitter import EmitterMixin
from .expressions import ExpressionMixin
from .loader import LoaderMixin
from .parser import ParserMixin
from .typecheck import TypesMixin


class DimScriptCompiler(LoaderMixin, ParserMixin, TypesMixin, ExpressionMixin,
                        EmitterMixin, CodegenMixin):
    def __init__(self):
        self.objects = {}        # class -> {field: (visibility, type, default)}
        self.methods = {}        # class -> {name: (visibility, static, params, ret, body)}
        self.vars = {}           # globals: name -> (visibility, type, default)
        self.functions = {}      # free functions: name -> (visibility, params, body)
        self.func_ret = {}       # name -> return type ('num', 'str', class, 'void')
        self.imports = {}        # namespace -> import path
        self.top = []
        self.lines = []
        self.errors = 0
        self.warnings = 0
        self.warned = set()
        self.output = []
        self.indent = 0
        self.scope = {}
        self.blocks = []
        self.cur_class = None
        self.sources = []
        self._parsed_classes = set()

    def _error(self, msg):
        self.errors += 1
        print(f"DimScript error: {msg}", file=sys.stderr)

    def _warn(self, msg):
        if msg in self.warned:
            return
        self.warned.add(msg)
        self.warnings += 1
        print(f"DimScript warning: {msg}", file=sys.stderr)

    def compile(self, sources, output):
        if not self._load(sources):
            return False
        self.parse()
        try:
            self.generate()
        except Exception as exc:  # noqa: BLE001
            self._error(f"internal {exc}")
            return False
        with open(output, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.output) + '\n')
        return self.errors == 0
