"""DimScript v2 compiler: Lua syntax with Java/C# strictness.

Package layout, one stage per module:

    tables.py       types, built-ins, engine variables, namespaces
    lexer.py        literals, brackets, strings, comments, splits
    loader.py       source files and imports
    parser.py       classes, functions, globals, blocks
    typecheck.py    C types, expression types, strictness checks
    expressions.py  interpolation, logic, fields, calls, methods
    emitter.py      statements, blocks, loops, assignments
    codegen.py      return inference and the game.c layout
    compiler.py     DimScriptCompiler, which joins the stages
    cli.py          python -m dimscript file.ds [-o output.c]

Grammar, where anything not listed is a compile error:

    import Dim.System.Gui            -- module or namespace import
    -- comment                       -- double dash only, as in Lua

    class Player                     -- class: fields, methods, constructor
        private number hp = 10       -- field with a modifier and a type
        public string nick = "bot"
        public function new()        -- constructor, runs after the fields
            self.hp = 10
        end
        public function heal(number amount) -> void
            self.hp = self.hp + amount
        end
        public static function make() -> Player
            return new Player()
        end
    end

    public number score = 0          -- module global with a modifier and a type
    private string title = "game"

    public function tick(number dt) -> void   -- free function
        local number step = dt * 2            -- local variable
        local Player p = new Player()         -- the type may be left out
        p.heal(step)
        Gui.text(24, $"Balance: {score}")     -- C#-style string interpolation
        if p.hp > 0 and not p.dead then       -- Lua blocks: then / do / end
            score += 1
        end
        for i = 1 to 3 do
            ds_log(i)
        end
    end

    local Player g = new Player()    -- top level statements (ds_main)
    g.heal(1)

Strictness: an undeclared variable, an unknown name or call, a wrong argument
count, a type mismatch (number, string, class), a private field of another class
and a missing class field are compile errors rather than the silently skipped
line of v1.
"""

from .compiler import DimScriptCompiler
from .lexer import (
    _COMPOUND_OPS, _FOR_RE, _LHS_RE, _NAME, _NUM_RE, find_assign,
    find_compound, interp_holes, num_value, open_parens, scan, split_top,
    strip_comment, sub_unquoted, used_outside_strings,
)
from .tables import (
    BUILTINS, ENGINE_VARS, FUNCTION_MAP, NATIVE_MATH, STD_NAMESPACES,
    STR_BUILTINS, TYPES, _MATH_NS, _MODS, _TYPE_ALIAS, canon_type,
)

__all__ = [
    'DimScriptCompiler', 'BUILTINS', 'ENGINE_VARS', 'FUNCTION_MAP',
    'NATIVE_MATH', 'STD_NAMESPACES', 'STR_BUILTINS', 'TYPES', '_MATH_NS',
    '_MODS', '_TYPE_ALIAS', 'canon_type', '_COMPOUND_OPS', '_FOR_RE',
    '_LHS_RE', '_NAME', '_NUM_RE', 'find_assign', 'find_compound',
    'interp_holes', 'num_value', 'open_parens', 'scan', 'split_top',
    'strip_comment', 'sub_unquoted', 'used_outside_strings',
]
