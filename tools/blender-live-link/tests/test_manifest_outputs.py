# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026 Blazing Storm contributors
# $/LicenseInfo$

"""Exercise the real manifest's Actions output helper without importing viewer deps."""
import ast
import os
from pathlib import Path
import secrets
import tempfile
import unittest
from unittest.mock import patch


MANIFEST = Path(__file__).resolve().parents[3] / "indra" / "newview" / "viewer_manifest.py"


def manifest_output_writer():
    tree = ast.parse(MANIFEST.read_text(encoding="utf-8"), filename=str(MANIFEST))
    viewer = next(node for node in tree.body
                  if isinstance(node, ast.ClassDef) and node.name == "ViewerManifest")
    method = next(node for node in viewer.body
                  if isinstance(node, ast.FunctionDef) and node.name == "set_github_output")
    # Load this *exact method* with no simulator or packaging dependencies.
    cls = ast.ClassDef(name="ManifestOutputWriter", bases=[], keywords=[],
                       body=[method], decorator_list=[])
    isolated = ast.fix_missing_locations(ast.Module(body=[cls], type_ignores=[]))
    namespace = {"os": os, "secrets": secrets, "ManifestError": RuntimeError}
    exec(compile(isolated, str(MANIFEST), "exec"), namespace)
    return namespace["ManifestOutputWriter"]()


class TestManifestOutputs(unittest.TestCase):
    def test_actions_outputs(self):
        writer = manifest_output_writer()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "github-output"
            with patch.dict(os.environ, {"GITHUB_ACTIONS": "true", "GITHUB_OUTPUT": str(output)}):
                writer.set_github_output("velopack_pack_id", "BlazingStorm")
                writer.set_github_output("velopack_pack_version", "1.2.3-4")
                writer.set_github_output("multiline", "line one\nline two")
            result = output.read_text(encoding="utf-8")
            self.assertIn("velopack_pack_id=BlazingStorm\n", result)
            self.assertIn("velopack_pack_version=1.2.3-4\n", result)
            self.assertIn("multiline<<", result)
            self.assertIn("line one\nline two\n", result)

    def test_missing_ci_output_is_error(self):
        writer = manifest_output_writer()
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}, clear=True):
            with self.assertRaises(RuntimeError):
                writer.set_github_output("velopack_pack_id", "BlazingStorm")


if __name__ == "__main__":
    unittest.main()
