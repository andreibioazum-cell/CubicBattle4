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
        body = self._fn_body(generated, "static void ds_fn_draw(const double x,")
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
        body = self._fn_body(generated, "static void ds_fn_update(const double t,")
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
        body = self._fn_body(generated, "static void ds_fn_draw(const double x)")
        self.assertNotIn("(void)x;", body)

    def test_read_only_params_are_const_written_params_are_not(self):
        generated = self.compile_source(
            """
            fn move(x, y) {
                x = x + 1
                rect(x, y, 10, 10)
            }
            """
        )
        # `x` is assigned, so it stays mutable; `y` is only read -> const.
        self.assertIn("static void ds_fn_move(double x, const double y)", generated)

    def test_empty_main_is_not_emitted(self):
        generated = self.compile_source(
            """
            fn init() {
                cls(0xFFFFFF)
            }
            """
        )
        self.assertNotIn("ds_main", generated)
        self.assertIn("ds_fn_init();", generated)

    def test_main_is_emitted_when_global_initialisers_exist(self):
        generated = self.compile_source(
            """
            num level = 3 * 2
            fn init() {
                cls(0xFFFFFF)
            }
            """
        )
        self.assertIn("static int ds_main(void) {", generated)
        self.assertIn("level = 3 * 2;", generated)
        self.assertIn("ds_main();", generated)

    def test_touch_hook_casts_to_declared_param_types(self):
        generated = self.compile_source(
            """
            fn touch(x, y, int action) {
                ds_log("touched")
            }
            """
        )
        self.assertIn("ds_fn_touch((double)x, (double)y, (int)action);", generated)

    def test_object_access_rewrite_skips_string_literals(self):
        # `player.` -> `player->` must not touch the texture name "player.png".
        generated = self.compile_source(
            """
            object Player {
                num x = 0
            }
            Player player = new Player()
            fn draw() {
                tex(player.x, 0, "player.png", 0, 1)
            }
            """
        )
        self.assertIn('tex(player->x, 0, "player.png", 0, 1)', generated)
        self.assertNotIn("player->png", generated)

    def test_ink_width_calls_are_emitted_as_is(self):
        generated = self.compile_source(
            """
            fn draw() {
                text("AB", screen_w / 2 - text_ink_width("AB") / 2, 10, 0xFFFFFF)
                text("CD", 0, 0, 0xFFFFFF)
            }
            """
        )
        self.assertIn('text("AB", screen_w / 2 - text_ink_width("AB") / 2, 10, 0xFFFFFF);', generated)

    def test_col_maps_to_uint32_t(self):
        generated = self.compile_source(
            """
            col bg = 0x1a1a2e
            fn draw() {
                cls(bg)
            }
            """
        )
        self.assertIn("uint32_t bg = 0x1a1a2e;", generated)

    def test_int_variables_stay_integers(self):
        generated = self.compile_source(
            """
            int lives = 3
            fn update() {
                lives = lives - 1
            }
            """
        )
        self.assertIn("int lives = 3;", generated)
        self.assertIn("lives = lives - 1;", generated)


if __name__ == "__main__":
    unittest.main()
