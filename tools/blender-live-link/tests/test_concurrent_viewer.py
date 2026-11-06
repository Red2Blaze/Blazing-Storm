"""Static regression checks for the viewer's simultaneous Live Link behavior.

Runs without the Windows/macOS viewer SDK; a separate C++ build is still
required for compile and runtime validation.
"""
from pathlib import Path
import unittest

VIEWER = Path(__file__).resolve().parents[3] / "indra/newview/blazingstorm/blender/bsblenderlivelink.cpp"


class TestConcurrentViewerPosing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = VIEWER.read_text(encoding="utf-8")

    def test_preview_does_not_reject_active_poser_subscription(self):
        self.assertNotIn("if (active) return;", self.source)
        self.assertNotIn("if (poser_streaming) return;", self.source)
        self.assertNotIn("if (!poser_streaming || active) return;", self.source)

    def test_edited_poser_joint_wins_over_blender_preview(self):
        self.assertIn("bool poser_controls_joint(", self.source)
        self.assertIn("if (link.poser_controls_joint(avatar, entry.first))", self.source)
        self.assertIn("joint_pose->getJointModified()", self.source)

    def test_poser_stream_uses_its_own_motion_target(self):
        self.assertIn("user_pose->getTargetRotation()", self.source)
        self.assertIn("user_pose->getTargetPosition()", self.source)
        self.assertIn('message["type"] = stream ? "poser_frame" : "pose_snapshot"', self.source)

    def test_stream_subscription_expires_and_never_opens_public_listener(self):
        self.assertIn("POSER_STREAM_TIMEOUT", self.source)
        self.assertIn("poser_last_subscribe", self.source)
        self.assertIn("address_v4::loopback()", self.source)


if __name__ == "__main__":
    unittest.main()
