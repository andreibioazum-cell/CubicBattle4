import os
import tempfile
import unittest
from pathlib import Path

from ds_compiler import DimScriptCompiler


class IncludeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, relative_path, content):
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def compile(self, sources):
        output = self.root / "game.c"
        compiler = DimScriptCompiler()
        success = compiler.compile([os.fspath(path) for path in sources], output)
        generated = output.read_text(encoding="utf-8") if output.exists() else ""
        return success, compiler, generated

    def test_nested_relative_includes_are_loaded_once(self):
        shared = self.write(
            "modules/shared.ds",
            """
            num shared_value = 7
            fn shared() {
                printn(shared_value)
            }
            """,
        )
        player = self.write(
            "modules/player.ds",
            """
            #include "shared.ds"
            fn player_update() {
                shared()
            }
            """,
        )
        entry = self.write(
            "game.ds",
            """
            #include "modules/player.ds"
            #include "modules/shared.ds" // duplicate on purpose
            fn update() {
                player_update()
            }
            """,
        )

        # Directory mode may pass a file that the entry point included already.
        success, compiler, generated = self.compile([entry, player, shared])

        self.assertTrue(success)
        self.assertEqual(3, len(compiler.loaded_sources))
        self.assertEqual(1, generated.count("static void ds_fn_shared(void) {"))
        self.assertEqual(1, generated.count("static void ds_fn_player_update(void) {"))
        self.assertIn("ds_fn_shared();", generated)
        self.assertIn("ds_fn_player_update();", generated)
        self.assertIn("ds_set_asset_manager(assets);", generated)

    def test_include_cycle_is_an_error(self):
        first = self.write("first.ds", '#include "second.ds"\n')
        self.write("second.ds", '#include "first.ds"\n')

        success, compiler, generated = self.compile([first])

        self.assertFalse(success)
        self.assertGreater(compiler.errors, 0)
        self.assertEqual("", generated)

    def test_missing_include_is_an_error(self):
        entry = self.write("game.ds", '#include "missing.ds"\n')

        success, compiler, generated = self.compile([entry])

        self.assertFalse(success)
        self.assertGreater(compiler.errors, 0)
        self.assertEqual("", generated)

    def test_comments_do_not_cut_double_slashes_inside_strings(self):
        entry = self.write(
            "game.ds",
            """
            str URL = "https://example.test/image.png" // real comment
            fn init() {
                prints(URL)
            }
            """,
        )

        success, _, generated = self.compile([entry])

        self.assertTrue(success)
        self.assertIn('const char * URL = "https://example.test/image.png";', generated)


if __name__ == "__main__":
    unittest.main()
