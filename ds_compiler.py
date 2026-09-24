"""Compatibility entry point of the DimScript compiler.

The compiler itself lives in the dimscript/ package, one stage per module (see
dimscript/__init__.py for the layout and the grammar). This file only keeps the
old import path and command line working:

    from ds_compiler import DimScriptCompiler
    python ds_compiler.py file.ds [-o output.c]
"""

from dimscript import (
    DimScriptCompiler,
    BUILTINS, ENGINE_VARS, FUNCTION_MAP, NATIVE_MATH, STD_NAMESPACES,
    STR_BUILTINS, TYPES, _MATH_NS, _MODS, _TYPE_ALIAS, canon_type,
    _COMPOUND_OPS, _FOR_RE, _LHS_RE, _NAME, _NUM_RE, find_assign, find_compound,
    interp_holes, num_value, open_parens, scan, split_top, strip_comment,
    sub_unquoted, used_outside_strings,
)
from dimscript.cli import main

__all__ = [
    'DimScriptCompiler', 'main', 'BUILTINS', 'ENGINE_VARS', 'FUNCTION_MAP',
    'NATIVE_MATH', 'STD_NAMESPACES', 'STR_BUILTINS', 'TYPES', '_MATH_NS',
    '_MODS', '_TYPE_ALIAS', 'canon_type', '_COMPOUND_OPS', '_FOR_RE',
    '_LHS_RE', '_NAME', '_NUM_RE', 'find_assign', 'find_compound',
    'interp_holes', 'num_value', 'open_parens', 'scan', 'split_top',
    'strip_comment', 'sub_unquoted', 'used_outside_strings',
]

if __name__ == '__main__':
    main()
