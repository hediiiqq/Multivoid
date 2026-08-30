// coop/water_sync.h -- bucket fill state and sponge wetness sync.
// Protocol v148, ReliableKind::WaterState (128) / WaterStatePayload.
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick change POLL,
// the inbound apply, the Key->actor index (via shared ScanHub), the deferred-apply retry,
// and the connect-snapshot. Talks to the engine ONLY through ue_wrap::water_prop.
//
// Model (HOST-AUTHORITATIVE ARBITRATION, host connect-snapshot adopt=1):
// - A client observes its local water change (e.g. pouring/dipping) and SENDS it as a request
//   to the host over ReliableKind::WaterState.
// - The host arbitrates: validates range per class (bucket 0..38, mop bucket 0..33, sponge 0..1),
//   applies to the authoritative instance, updates its baseline, and broadcasts the authoritative
//   value to ALL peers INCLUDING the origin.
// - The client applies the host's authoritative value verbatim (even when it corrects a local pour)
//   and sets its local baseline to the applied value. Because apply sets baseline == current,
//   subsequent polls detect zero delta, structurally preventing ping-pong / re-send loops.
// - The host snapshots canonical water state on connect with adopt=1 so joiners adopt the
//   host's world verbatim.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WaterStatePayload;
}  // namespace coop::net

namespace coop::water_sync {

// Resolve the bucket and sponge classes + register with the shared scan hub.
// Idempotent; retried every net-pump tick until BP classes are loaded. Stores session pointer.
// Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a WaterStatePayload packet arrived.
// On host: client request -> validate, apply, update baseline, and broadcast authoritative value to all peers.
// On client: authoritative update -> apply verbatim, update baseline (no ping-pong).
// Defers if the instance has not streamed in yet. Called from event_dispatch_state reliable drain.
void OnReliable(const coop::net::WaterStatePayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot current liquid fill / wetness of every indexed water prop to a freshly
// connected client `peerSlot` with adopt=1. Called from net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick: poll for live changes (broadcast updates / client requests) + retry deferred applies (throttled).
// Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear per-session poll baseline + pending applies.
void OnDisconnect();

}  // namespace coop::water_sync
