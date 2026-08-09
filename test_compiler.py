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
                    prints("i=" + i)
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


if __name__ == "__main__":
    unittest.main()
