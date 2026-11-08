# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026 Blazing Storm contributors
# $/LicenseInfo$

"""Static regression checks for the viewer's simultaneous Live Link behavior.

Runs without the Windows/macOS viewer SDK; a separate C++ build is still
required for compile and runtime validation.
"""
from pathlib import Path
import unittest

VIEWER = Path(__file__).resolve().parents[3] / "indra/newview/blazingstorm/blender/bsblenderlivelink.cpp"
POSER_HEADER = Path(__file__).resolve().parents[3] / "indra/newview/fsposeranimator.h"


class TestConcurrentViewerPosing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = VIEWER.read_text(encoding="utf-8")

    def test_preview_does_not_reject_active_poser_subscription(self):
        self.assertNotIn("if (active) return;", self.source)
        self.assertNotIn("if (poser_streaming) return;", self.source)
        self.assertNotIn("if (!poser_streaming || active) return;", self.source)

    def test_poser_owns_joint_until_blender_emits_new_edit(self):
        self.assertIn("bool poser_controls_joint(", self.source)
        self.assertIn("if (link.poser_controls_joint(avatar, entry.first))", self.source)
        self.assertIn("joint_pose->getJointModified()", self.source)
        self.assertIn("blender_owned.count(name) == 0", self.source)
        self.assertIn('obj->if_contains("blender_edits")', self.source)
        self.assertIn("blender_edit_revisions", self.source)
        self.assertIn("blender_owned.insert(entry.first)", self.source)
        self.assertIn("observe_poser_edits(avatar)", self.source)
        self.assertIn("blender_owned.erase(name)", self.source)

    def test_poser_stream_uses_its_own_motion_target(self):
        self.assertIn("pose->getTargetRotation()", self.source)
        self.assertIn("pose->getTargetPosition()", self.source)
        self.assertIn('message["poser_active"] = poser_active', self.source)
        self.assertIn('message["edited_count"]', self.source)
        self.assertIn("motion->currentlyPosingJoint(pose)", self.source)
        self.assertIn("pose->getJointModified()", self.source)
        self.assertIn('message["type"] = stream ? "poser_frame" : "pose_snapshot"', self.source)

    def test_viewer_reads_poser_motion_through_public_api(self):
        # The original receiver directly called FSPoserAnimator's private
        # getPosingMotion(), which fails MSVC with C2248.
        header = POSER_HEADER.read_text(encoding="utf-8")
        bridge = header.find("getPosingMotionForLiveLink(LLVOAvatar* avatar) const")
        private = header.find("  private:", bridge)
        self.assertGreaterEqual(bridge, 0)
        self.assertGreater(private, bridge)
        self.assertEqual(
            self.source.count("poser.getPosingMotionForLiveLink(avatar)"), 5)
        self.assertNotIn("poser.getPosingMotion(avatar)", self.source)

    def test_blender_handoff_rebases_poser_without_generating_user_edits(self):
        self.assertIn("setLiveLinkTargetRotation", self.source)
        self.assertIn("setLiveLinkTargetPosition", self.source)
        self.assertIn("restore_poser_targets(avatar)", self.source)
        self.assertIn("original_poser_targets.erase(name)", self.source)
        pose_cpp = (VIEWER.parents[2] / "fsjointpose.cpp").read_text(encoding="utf-8")
        silent_rotation = pose_cpp.split("void FSJointPose::setLiveLinkTargetRotation")[1].split("void FSJointPose::setLiveLinkTargetPosition")[0]
        self.assertNotIn("addStateToUndo", silent_rotation)
        self.assertIn("mCurrentState.setLiveLinkTargetRotation(target)", silent_rotation)
        self.assertNotIn("mCurrentState.mBaseRotation", silent_rotation)
        pose_header = (VIEWER.parents[2] / "fsjointpose.h").read_text(encoding="utf-8")
        self.assertIn("void setLiveLinkTargetRotation(const LLQuaternion& target)", pose_header)
        self.assertIn("void setLiveLinkTargetPosition(const LLVector3& target)", pose_header)
        # Private base transforms may only be used inside FSJointState.
        self.assertIn("mRotation.set(target * ~mBaseRotation)", pose_header)
        self.assertIn("mPosition.set(target - mBasePosition)", pose_header)

    def test_live_link_updates_visible_poser_sliders_without_user_commits(self):
        poser = (VIEWER.parents[2] / "fsfloaterposer.cpp").read_text(encoding="utf-8")
        poser_h = (VIEWER.parents[2] / "fsfloaterposer.h").read_text(encoding="utf-8")
        joint_h = (VIEWER.parents[2] / "fsjointpose.h").read_text(encoding="utf-8")
        joint_cpp = (VIEWER.parents[2] / "fsjointpose.cpp").read_text(encoding="utf-8")
        draw = poser.split("void FSFloaterPoser::draw()", 1)[1].split(
            "void FSFloaterPoser::markSelectedJointsToHighlight", 1)[0]
        self.assertIn("getLiveLinkTargetRevision()", draw)
        self.assertIn("refreshRotationSlidersAndSpinners()", draw)
        self.assertIn("refreshPositionSlidersAndSpinners()", draw)
        self.assertIn("refreshTrackpadCursor()", draw)
        self.assertNotIn("onYawPitchRollChanged(", draw)
        self.assertNotIn("setJointRotation(", draw)
        self.assertIn("mLastLiveLinkRevision", poser_h)
        self.assertIn("mLiveLinkTargetRevision", joint_h)
        self.assertIn("++mLiveLinkTargetRevision", joint_cpp)
        self.assertIn("difference < 0.00000001f", joint_cpp)

    def test_stream_subscription_expires_and_never_opens_public_listener(self):
        self.assertIn("POSER_STREAM_TIMEOUT", self.source)
        self.assertIn("poser_last_subscribe", self.source)
        self.assertIn("address_v4::loopback()", self.source)


if __name__ == "__main__":
    unittest.main()
