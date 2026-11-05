# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026 Blazing Storm contributors
# $/LicenseInfo$

import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'blazing_live_link'))
from protocol import build_packet, parse_ack, parse_snapshot, PROTOCOL, VERSION, MAX_BYTES


class TestProtocol(unittest.TestCase):
    session = "0" * 32

    def test_pose_roundtrip(self):
        packet = json.loads(build_packet("pose", self.session, 1, 12, 24, {"mHead": [1, 0, 0, 0]}))
        self.assertEqual(packet["joints"]["mHead"], [1, 0, 0, 0])
        self.assertEqual(packet["frame"], 12)
        self.assertEqual(packet["type"], "pose")

    def test_stop(self):
        packet = json.loads(build_packet("stop", self.session, 2))
        self.assertNotIn("joints", packet)

    def test_ack(self):
        ack = json.dumps({"protocol": PROTOCOL, "version": VERSION, "type": "ack", "session": self.session}).encode()
        self.assertTrue(parse_ack(ack, self.session))
        self.assertFalse(parse_ack(ack, "1" * 32))
        self.assertFalse(parse_ack(b"not-json", self.session))

    def test_limit_and_invalid_values(self):
        with self.assertRaises(ValueError):
            build_packet("pose", self.session, 0, joints={"mPelvis": [float('nan'), 0, 0, 0]})
        with self.assertRaises(ValueError):
            build_packet("pose", self.session, 0, joints={str(i): [1, 0, 0, 0] for i in range(257)})
        with self.assertRaises(ValueError):
            build_packet("pose", "bad", 0, joints={})

    def test_bone_translations_are_opt_in(self):
        message = build_packet(
            "pose", self.session, 2, frame=10,
            joints={"mHead": [1, 0, 0, 0]},
            positions={"mHead": [0.01, -0.02, 0.0]},
        )
        packet = json.loads(message)
        self.assertEqual(packet["positions"]["mHead"], [0.01, -0.02, 0.0])
        with self.assertRaises(ValueError):
            build_packet("pose", self.session, 1, joints={}, positions={"mHead": [0, 0]})
        with self.assertRaises(ValueError):
            build_packet("pose", self.session, 1, joints={}, positions={"mHead": [float("inf"), 0, 0]})

    def test_snapshot_request_and_response(self):
        message = json.loads(build_packet("snapshot_request", self.session, 9))
        self.assertEqual(message["type"], "snapshot_request")
        self.assertNotIn("joints", message)
        snapshot = json.dumps({
            "protocol": PROTOCOL, "version": VERSION, "type": "pose_snapshot",
            "session": self.session,
            "joints": {"mHead": [0.9, 0, 0.1, 0]},
            "positions": {"mPelvis": [0, 0, 0.15]},
        }).encode("ascii")
        parsed = parse_snapshot(snapshot, self.session)
        self.assertEqual(parsed["positions"]["mPelvis"][2], 0.15)
        self.assertIsNone(parse_snapshot(snapshot, "1" * 32))
        self.assertIsNone(parse_snapshot(b"not-json", self.session))
        self.assertIsNone(parse_snapshot(snapshot.replace(b"mHead", b"Head_"), self.session))

    def test_typical_bento_pose_fits(self):
        joints = {"mBone" + str(i): [0.999999, 0.00001, 0.00002, 0.00003] for i in range(150)}
        self.assertLessEqual(len(build_packet("pose", self.session, 1, joints=joints)), MAX_BYTES)


if __name__ == '__main__':
    unittest.main()
