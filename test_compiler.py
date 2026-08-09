#!/usr/bin/env python3
"""Regression tests for the DimScript source-to-C front end."""

import tempfile
import unittest
from pathlib import Path

from ds_compiler import DimScriptCompiler


class CompilerRegressionTests(unittest.TestCase):
    def compile_source(self, source: str) -> str:
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "test.ds"
            output_path = Path(directory) / "test.c"
            source_path.write_text(source, encoding="utf-8")
            compiler = DimScriptCompiler()
            self.assertTrue(compiler.compile([source_path], output_path))
            return output_path.read_text(encoding="utf-8")

    def test_for_and_string_addition_are_emitted_as_c(self):
        generated = self.compile_source(
            """
            num i = 0
            fn update() {
                for (i = 0; i <= 10; i += 1) {
                    ds_log("i=" + i)
                }
            }
            """
        )
        self.assertIn("for (i = 0; i <= 10; i += 1)", generated)
        self.assertIn("ds_concat(\"i=\"", generated)
        self.assertNotIn("ds_str_cat", generated)

    def test_objects_use_structs_and_direct_fields(self):
        generated = self.compile_source(
            """
            object Player {
                num x = 0
                fn move(num dx) {
                    self.x += dx
                }
            }
            fn init() {
                Player player = new Player()
                player.move(2)
            }
            """
        )
        self.assertIn("struct Player", generated)
        self.assertIn("self->x += dx", generated)
        self.assertIn("ds_obj_Player_move(player, 2)", generated)
        self.assertNotIn("ds_read(", generated)

    def _fn_body(self, generated, signature):
        """Return the body of the first generated static fn with this signature."""
        start = generated.index(signature)
        brace = generated.index('{', start)
        close = generated.index('\n}\n', brace)
        return generated[brace:close]

    def test_only_unused_parameters_get_void_cast(self):
        generated = self.compile_source(
            """
            fn draw(x, y, w, h, size) {
                rect(x, y, w, h)
            }
            """
        )
        body = self._fn_body(generated, "static void ds_fn_draw(double x,")
        # Used parameters must not be cast to (void) (dead code).
        for name in ("x", "y", "w", "h"):
            self.assertNotIn(f"(void){name};", body, f"{name} is used")
        # A genuinely unused parameter still suppresses the warning.
        self.assertIn("(void)size;", body)

    def test_used_parameter_is_not_reported_by_similar_identifier(self):
        # `x` inside `btn_x` or inside a string literal is not a use of `x`,
        # so the parameter genuinely needs the warning suppressed.
        generated = self.compile_source(
            """
            num btn_x = 10
            fn update(t, x) {
                btn_x = btn_x + 1
                ds_log("x")
            }
            """
        )
        body = self._fn_body(generated, "static void ds_fn_update(double t,")
        self.assertIn("(void)t;", body)
        self.assertIn("(void)x;", body)
        # ...a genuinely used parameter keeps its (void) cast off.
        generated = self.compile_source(
            """
            fn draw(x) {
                circle(x, x, 5, 0xFF0000)
            }
            """
        )
        body = self._fn_body(generated, "static void ds_fn_draw(double x)")
        self.assertNotIn("(void)x;", body)


if __name__ == "__main__":
    unittest.main()
