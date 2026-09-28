# Blazing Storm remote relay design

## Goal

Prevent the Controller and Subject from learning each other's public IP address.

The relay is not a remote-permission authority. Both viewers connect outward to
the relay and all commands still execute through the Subject-side permission and
validation code.

## Connection flow

1. Subject viewer opens a TLS WebSocket to the relay and creates an ephemeral
   relay session.
2. Relay returns a session ID and two random role tokens.
3. Subject keeps the Subject token private and sends the Controller token through
   the existing visible Second Life possession bootstrap IM.
4. Controller opens its own TLS WebSocket to the relay and joins with that token.
5. Relay announces that both peers are present.
6. Existing Blazing Storm protocol frames are carried through the relay.
7. Either peer disconnecting destroys the relay room and closes the other socket.

No peer address is included in any relay response or forwarded frame.

## Security decisions

- Production transport is WSS/TLS. Plain WS is development-only.
- Tokens are random 256-bit values and compared in constant time.
- Tokens are sent in the WebSocket handshake message, not the URL, to avoid
  leaking them through ordinary URL/access logs.
- The relay forwards application payloads and does not interpret permissions.
- Subject-side command validation remains mandatory.
- Sessions are ephemeral and in-memory for the first implementation.
- Any peer disconnect fails closed and ends the relay room.
- Money has no Blazing Storm permission or remote command and remains outside
  Full Control.

## Future hardening

Before calling the Internet transport production-ready:

- Add the viewer-side WSS `RelayTransport`.
- Pin/validate normal TLS certificates using Firestorm's supported trust path.
- Add application-level end-to-end encryption if relay operators should also be
  unable to read possession traffic.
- Add reconnect/resume only if it can preserve fail-closed semantics.
- Add rate limits and abuse controls around session creation.
- Use Redis/Azure SignalR (or sticky routing) before running multiple relay
  replicas.
- Add protocol version negotiation.
- Add automated relay integration tests using two WebSocket clients.


## Viewer integration

The viewer-side relay implementation lives on `blazing/relay-viewer`.

Both viewers configure the same trusted relay endpoint locally, for example:

`wss://relay.example.com/v1/relay`

The Controller never supplies a relay URL to the Subject. This is deliberate:
allowing an untrusted Controller to select the relay would let it direct the
Subject to an attacker-controlled server and reveal the Subject's public IP.

The existing Controller-first possession UX is preserved with a two-stage
Second Life IM bootstrap:

1. Controller sends a visible relay possession request IM containing its avatar
   UUID and a one-time nonce.
2. Subject recognizes the request, connects to its locally configured relay,
   and creates the relay room.
3. Subject receives `sessionId` and `controllerToken` from the relay.
4. Subject sends those two values plus the original nonce back to the requesting
   Controller through a visible Second Life IM.
5. Controller validates that the invite came from the requested Subject and that
   the nonce matches its outstanding request, then joins the same locally
   configured relay.
6. After both viewers receive `{"type":"peer","state":"connected"}`, the
   Controller sends the existing Blazing Storm `REQUEST|...` frame through the
   relay.
7. The Subject validates the Controller avatar UUID and nonce again and then
   uses the existing trusted-controller auto-accept or manual Accept/Decline
   flow.
8. All existing `CMD|...`, `EVT|...`, `ACCEPT`, `REJECT`, `WAIT`,
   `BLOCKED`, `APPLIED`, and `ENDED` application frames are carried as
   individual WebSocket text messages.

Once the relay announces that the peer is connected, relay-control JSON is no
longer interpreted by the viewer. Subsequent payloads are treated only as peer
application frames, preventing a connected peer from spoofing relay control
messages.

The WSS client runs networking on its own thread. Subject session state, command
dispatch, permissions, inventory, camera, teleport, movement, and object
interaction remain on the viewer thread.

### Viewer configuration and fallback

`BlazingStormRelayUrl` is a per-account setting exposed at the top of the
Blazing Storm Remote Control floater.

- Non-empty `wss://...`: possession requests use the Internet relay.
- Empty: the existing `127.0.0.1` transport remains available for local debug
  testing.
- The explicit **Debug: Open Listener** button remains local-only.

Production relay mode requires WSS with normal certificate and hostname
validation. The current viewer imports the Windows trusted root store into its
OpenSSL context on Windows and uses OpenSSL's default trust paths elsewhere.

### Viewer relay test checklist

- Configure the same deployed WSS relay URL on two different machines/networks.
- Send a possession request and verify the Subject creates the room and the
  Controller joins without either peer connecting directly to the other.
- Test manual Accept and Decline, then a trusted-controller auto-accept.
- Exercise movement, chat, IM, touch/sit/dialogs, camera, teleport, and inventory
  over relay.
- Revoke permissions while connected and verify Subject-side rejection remains
  authoritative.
- Disconnect either viewer and verify the other side immediately ends the
  possession session.
- Kill the relay process/network path and verify both viewers fail closed.
- Retry a second possession session after disconnect to verify no stale room,
  token, nonce, dialog, inventory, or controller state remains.
- Use an invalid certificate/hostname and verify the viewer refuses the WSS
  connection.
