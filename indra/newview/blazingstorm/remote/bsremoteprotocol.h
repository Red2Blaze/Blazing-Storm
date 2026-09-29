/**
 * @file bsremoteprotocol.h
 * @brief Transport-neutral messages for Blazing Storm possession sessions.
 */

#ifndef BS_REMOTE_PROTOCOL_H
#define BS_REMOTE_PROTOCOL_H

#include <cstdint>
#include <string>

namespace BlazingStorm
{
    enum class RemoteRole
    {
        NoneValue,
        Host,
        Controller
    };

    enum class RemoteCommandType
    {
        NoneValue,
        MoveForward,
        MoveBackward,
        StrafeLeft,
        StrafeRight,
        TurnLeft,
        TurnRight,
        MoveUp,
        MoveDown,
        FlyOn,
        FlyOff,
        ToggleFly,
        Jump,
        StopForward,
        StopStrafe,
        StopTurn,
        StopVertical,
        Stop,
        Say,
        SendInstantMessage,
        SitObject,
        Stand,
        TouchObject,
        DialogReply,
        RestrictMovementOn,
        RestrictMovementOff,
        RestrictNearbyChatOn,
        RestrictNearbyChatOff,
        RestrictInstantMessageOn,
        RestrictInstantMessageOff,
        EmergencyRelease,
        CameraLeft,
        CameraRight,
        CameraUp,
        CameraDown,
        CameraIn,
        CameraOut,
        CameraReset,
        InventoryBrowse,
        InventoryWear,
        InventoryRemove,
        InventoryRez,
        TeleportLocation,
        TeleportOffer,
        TeleportRequest,
        TeleportAccept,
        TeleportDecline,
        CameraFocus
    };

    struct RemoteCommand
    {
        RemoteCommandType type = RemoteCommandType::NoneValue;
        std::string targetId;
        std::string text;
        std::uint64_t sequence = 0;
    };

    struct PairingRequest
    {
        std::string code;
        std::string controllerId;
        std::string controllerName;
    };

    struct PairingResponse
    {
        bool accepted = false;
        std::string sessionId;
        std::string reason;
    };
}

#endif // BS_REMOTE_PROTOCOL_H
