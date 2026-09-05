"""Regression tests for leanoff's scheduling and failure handling.

Uses a stubbed `run_lean` (no real Lean toolchain needed) to exercise exactly
the orchestration layer: level scheduling, fail-fast on a failed level,
cancellation of queued work, and turning a killed lean process into a FAIL
result instead of a traceback.
"""

import sys
import tempfile
import time
import unittest
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import leanoff
from leanoff import Module, Result


def make_args(root, jobs=1):
    return Namespace(
        root=str(root), lean=str(root / "bin"), out=None, lean_path=[],
        filter=None, jobs=jobs, allow_sorry=False, format="json",
    )


def setup_project(files):
    tmp = Path(tempfile.mkdtemp())
    (tmp / "bin").mkdir()
    # find_toolchain resolves "lean" on POSIX and "lean.exe" on Windows —
    # create both so the stubbed orchestration tests run everywhere.
    (tmp / "bin" / "lean.exe").touch()
    (tmp / "bin" / "lean").touch()
    for name, text in files.items():
        f = tmp / name
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(text, encoding="utf-8")
    return tmp


class BuildFailFast(unittest.TestCase):
    def setUp(self):
        self._orig = leanoff.run_lean

    def tearDown(self):
        leanoff.run_lean = self._orig

    def _mods(self, tmp):
        mods = leanoff.discover_modules(tmp, None)
        known = {m.name for m in mods}
        for m in mods:
            leanoff.parse_imports(m, known)
        return mods

    def test_stops_after_failed_level(self):
        tmp = setup_project({
            "X.lean": "",
            "Y.lean": "",
            "Z.lean": "import X\n",
        })
        calls = []

        def fake(lean_exe, root, mod, lean_path, out_olean, cwd_root):
            calls.append(mod.name)
            if mod.name == "X":
                return Result(mod.name, False, 1, 0, 0, 0.0, "X.lean:1:0: error: nope")
            time.sleep(0.15)
            return Result(mod.name, True, 0, 0, 0, 0.0, "")

        leanoff.run_lean = fake
        code = leanoff.cmd_build(make_args(tmp, jobs=1))
        # X failed in level 0: queued Y is cancelled, Z (level 1) never runs,
        # and the whole thing exits non-zero without a traceback
        self.assertEqual(code, 1)
        self.assertIn("X", calls)
        self.assertNotIn("Z", calls)

    def test_healthy_build_runs_everything(self):
        tmp = setup_project({
            "A.lean": "",
            "B.lean": "import A\n",
            "C.lean": "import B\n",
        })
        calls = []

        def fake(lean_exe, root, mod, lean_path, out_olean, cwd_root):
            calls.append(mod.name)
            return Result(mod.name, True, 0, 0, 0, 0.01, "")

        leanoff.run_lean = fake
        code = leanoff.cmd_build(make_args(tmp, jobs=4))
        self.assertEqual(code, 0)
        self.assertEqual(sorted(calls), ["A", "B", "C"])

    def test_killed_lean_becomes_fail_result(self):
        tmp = setup_project({"A.lean": ""})

        def fake(lean_exe, root, mod, lean_path, out_olean, cwd_root):
            raise RuntimeError("process killed by OOM")

        leanoff.run_lean = fake
        code = leanoff.cmd_build(make_args(tmp, jobs=1))
        self.assertEqual(code, 1)

    def test_verify_reports_all_modules_even_with_failures(self):
        tmp = setup_project({
            "A.lean": "",
            "B.lean": "",
        })

        def fake(lean_exe, root, mod, lean_path, out_olean, cwd_root):
            ok = mod.name != "A"
            return Result(mod.name, ok, 0 if ok else 1, 0, 0, 0.01, "")

        leanoff.run_lean = fake
        code = leanoff.cmd_verify(make_args(tmp, jobs=2))
        self.assertEqual(code, 1)


class TopoLevels(unittest.TestCase):
    def test_level_order(self):
        mods = [
            Module("C", Path("C.lean"), ["B"]),
            Module("A", Path("A.lean"), []),
            Module("B", Path("B.lean"), ["A"]),
        ]
        levels = leanoff.topo_levels(mods)
        self.assertEqual([m.name for lvl in levels for m in lvl], ["A", "B", "C"])
        self.assertEqual(len(levels), 3)

    def test_parallel_modules_share_a_level(self):
        mods = [
            Module("A", Path("A.lean"), []),
            Module("B", Path("B.lean"), []),
            Module("C", Path("C.lean"), ["A", "B"]),
        ]
        levels = leanoff.topo_levels(mods)
        self.assertEqual(len(levels), 2)
        self.assertEqual({m.name for m in levels[0]}, {"A", "B"})
        self.assertEqual([m.name for m in levels[1]], ["C"])

    def test_cycle_exits_with_message(self):
        mods = [
            Module("A", Path("A.lean"), ["B"]),
            Module("B", Path("B.lean"), ["A"]),
        ]
        with self.assertRaises(SystemExit):
            leanoff.topo_levels(mods)


if __name__ == "__main__":
    unittest.main()
