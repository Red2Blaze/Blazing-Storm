"""Blazing Storm Live Link: real-time Avastar/SL deform-bone preview.

Blender 5.2-first, with a compatible Blender 3.6 build from the same source.
"""
bl_info = {
    "name": "Blazing Storm - Blender Live Link",
    "author": "Blazing Storm contributors",
    "version": (0, 1, 0),
    "blender": (5, 2, 0),
    "location": "View3D > Sidebar > Blazing",
    "description": "Stream locally evaluated Avastar deform-bone poses to Blazing Storm Viewer",
    "category": "Animation",
}

import math
import socket
import time
import uuid

import bpy
from bpy.props import BoolProperty, IntProperty, PointerProperty
from bpy.types import Operator, Panel, PropertyGroup

from .protocol import PORT, build_packet, parse_ack

_client = None


def is_armature(_self, obj):
    return obj is not None and obj.type == "ARMATURE"


class BSLLSettings(PropertyGroup):
    armature: PointerProperty(name="Avatar Rig", type=bpy.types.Object, poll=is_armature)
    rate: IntProperty(name="Update rate", default=24, min=5, max=60,
                      description="Maximum samples per second. Pose changes stream during playback and scrubbing")
    pelvis: BoolProperty(name="Pelvis translation (experimental)", default=False,
                         description="Transfer mPelvis translation; leave off until coordinate mapping is calibrated")


class Client:
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setblocking(False)
        self.session = uuid.uuid4().hex
        self.sequence = 0
        self.last_ack = 0.0
        self.last_sent = 0.0
        self.last_content = None
        self.error = ""
        self.running = True

    def send(self, wire):
        try:
            self.socket.sendto(wire, ("127.0.0.1", PORT))
            self.error = ""
            self.last_sent = time.monotonic()
        except OSError as ex:
            self.error = str(ex)

    def poll_ack(self):
        for _ in range(8):
            try:
                data, address = self.socket.recvfrom(1024)
            except (BlockingIOError, InterruptedError):
                break
            except OSError as ex:
                self.error = str(ex)
                break
            if address[0] == "127.0.0.1" and parse_ack(data, self.session):
                self.last_ack = time.monotonic()

    def close(self):
        self.sequence += 1
        try:
            self.send(build_packet("stop", self.session, self.sequence))
        finally:
            self.running = False
            self.socket.close()


def choose_rig(scene):
    settings = scene.bs_blender_live_link
    if settings.armature and settings.armature.type == 'ARMATURE':
        return settings.armature
    obj = bpy.context.object
    if obj and obj.type == 'ARMATURE':
        return obj
    return None


def evaluate_pose(armature, send_pelvis):
    """Sample evaluated deform joints, including results of Avastar IK/constraints.

    Rigid *rest-relative local* rotation is transmitted; unlike matrix_basis,
    this also captures constraint evaluation. No Avastar private API is used.
    The viewer still needs skeleton/axis calibration for exact SL playback.
    """
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = armature.evaluated_get(depsgraph)
    poses = {}
    pelvis = None
    for bone in evaluated.pose.bones:
        if not bone.name.startswith("m") or not bone.bone.use_deform:
            continue
        if bone.parent:
            local_pose = bone.parent.matrix.inverted_safe() @ bone.matrix
            local_rest = bone.parent.bone.matrix_local.inverted_safe() @ bone.bone.matrix_local
        else:
            local_pose = bone.matrix
            local_rest = bone.bone.matrix_local
        local_delta = local_rest.inverted_safe() @ local_pose
        rot = local_delta.to_quaternion().normalized()
        values = [round(float(n), 6) for n in (rot.w, rot.x, rot.y, rot.z)]
        if not all(math.isfinite(n) for n in values):
            continue
        poses[bone.name] = values
        if send_pelvis and bone.name == "mPelvis":
            pelvis = [round(float(v), 6) for v in local_delta.translation]
            if not all(math.isfinite(n) for n in pelvis):
                pelvis = None
    return poses, pelvis


def timer_tick():
    global _client
    client = _client
    if client is None or not client.running:
        return None
    client.poll_ack()
    try:
        scene = bpy.context.scene
        if scene is not None and hasattr(scene, "bs_blender_live_link"):
            settings = scene.bs_blender_live_link
            rig = choose_rig(scene)
            if rig:
                joints, pelvis = evaluate_pose(rig, settings.pelvis)
                # If nothing moves, keep a heartbeat so a paused pose stays active.
                content = (scene.frame_current, tuple(sorted(joints.items())),
                           tuple(pelvis) if pelvis else None)
                now = time.monotonic()
                if content != client.last_content or now - client.last_sent > 0.75:
                    client.sequence += 1
                    wire = build_packet("pose", client.session, client.sequence,
                                        scene.frame_current,
                                        scene.render.fps / (scene.render.fps_base or 1),
                                        joints, pelvis)
                    client.send(wire)
                    client.last_content = content
            else:
                client.error = "Select an Avastar armature"
            return 1.0 / settings.rate
    except (ValueError, RuntimeError, OSError, AttributeError, TypeError) as ex:
        client.error = str(ex)
    return 0.1


class BSLL_OT_start(Operator):
    bl_idname = "bsll.start"
    bl_label = "Start Live Link"
    bl_description = "Start sending poses to the local Blazing Storm viewer"

    def execute(self, context):
        global _client
        if _client is not None:
            return {'FINISHED'}
        if choose_rig(context.scene) is None:
            self.report({'ERROR'}, "Select the Avastar armature first")
            return {'CANCELLED'}
        _client = Client()
        if not bpy.app.timers.is_registered(timer_tick):
            bpy.app.timers.register(timer_tick, first_interval=0.01)
        self.report({'INFO'}, "Streaming to local viewer on 127.0.0.1")
        return {'FINISHED'}


class BSLL_OT_stop(Operator):
    bl_idname = "bsll.stop"
    bl_label = "Stop Live Link"

    def execute(self, context):
        global _client
        if _client is not None:
            _client.close()
            _client = None
        return {'FINISHED'}


class BSLL_PT_panel(Panel):
    bl_label = "Blazing Storm Live Link"
    bl_idname = "BSLL_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Blazing"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.bs_blender_live_link
        layout.prop(settings, "armature")
        layout.prop(settings, "rate")
        layout.prop(settings, "pelvis")
        if _client:
            layout.operator("bsll.stop", icon="CANCEL")
            if _client.last_ack and time.monotonic() - _client.last_ack < 3:
                layout.label(text="Viewer connected", icon="CHECKMARK")
            else:
                layout.label(text="Waiting for viewer", icon="INFO")
            if _client.error:
                layout.label(text=_client.error[:95], icon="ERROR")
        else:
            layout.operator("bsll.start", icon="PLAY")
        layout.label(text="Local only - no animation upload")


_classes = (BSLLSettings, BSLL_OT_start, BSLL_OT_stop, BSLL_PT_panel)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.bs_blender_live_link = PointerProperty(type=BSLLSettings)


def unregister():
    global _client
    if _client is not None:
        _client.close()
        _client = None
    if bpy.app.timers.is_registered(timer_tick):
        bpy.app.timers.unregister(timer_tick)
    del bpy.types.Scene.bs_blender_live_link
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
