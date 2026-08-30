// coop/interactables/water_sync.cpp -- see coop/interactables/water_sync.h.
// Bucket fill state and sponge wetness scalar sync.
//
// Model (HOST-AUTHORITATIVE ARBITRATION, host connect-snapshot adopt=1):
// - Clients observe local changes (pouring, dipping) and send requests to the host
//   over ReliableKind::WaterState (WaterStatePayload).
// - The host arbitrates: validates range per class (bucket 0..38, mop bucket 0..33, sponge 0..1),
//   applies to the authoritative instance, updates its baseline, and broadcasts the authoritative
//   value to ALL peers INCLUDING the origin.
// - Origin echo ensures all peers converge on the host's authoritative sequence without ping-pong:
//   the client's apply sets its local baseline == applied value, so subsequent polls detect zero delta.
// - Connect replay seeds current state with adopt=1 so joiners adopt the host's world verbatim.

#include "coop/interactables/water_sync.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/devices/water_prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"
#include "coop/element/object_scan_hub.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::water_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace WP = ue_wrap::water_prop;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL           = std::chrono::seconds(25);
constexpr auto kCapWarningInterval   = std::chrono::seconds(5);
constexpr float kWaterEps            = 0.005f;
constexpr size_t kMaxPending         = 512;

bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::window_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_echo{false};
std::atomic<bool> g_noSessionLogged{false};
std::chrono::steady_clock::time_point g_lastCapWarn{};

void LogPendingCapHit(size_t heldCount) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastCapWarn >= kCapWarningInterval) {
        g_lastCapWarn = now;
        UE_LOGW("water: pending queue cap hit (%zu entries held >= cap %zu) -- refusing new entry",
                heldCount, kMaxPending);
    }
}

struct Ref { void* actor; int32_t idx; };
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

std::mutex g_stateMutex;
std::unordered_map<std::wstring, float> g_lastKnown;

struct Pending { float value; bool adopt; uint8_t fromSlot; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash = 0;
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;

uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void* ResolveFast(const std::wstring& key) {
    if (!IndexCurrent()) return nullptr;
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) {
        return it->second.actor;
    }
    return nullptr;
}

// ---- Shared-scan hub consumer ----
std::vector<std::pair<std::wstring, Ref>> g_scanFound;

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
    if (!R::IsLive(obj)) return;
    std::wstring key = WP::GetKeyString(obj);
    if (key.empty() || key == L"None") return;
    g_scanFound.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (isFull) g_byKey.clear();
        for (auto& f : g_scanFound) g_byKey[f.first] = f.second;
        if (!isFull) {
            for (auto it = g_byKey.begin(); it != g_byKey.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_byKey.erase(it);
            }
        }
        for (auto& kv : g_byKey) keysHash ^= FnvKey(kv.first);
        total = g_byKey.size();
        g_indexGen = worldGen;
    }
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("water: index now %zu live keyed water prop(s), keysHash=0x%016llX (%s pass, +%zu new)",
                total, static_cast<unsigned long long>(keysHash), isFull ? "full" : "tail", added);
    }
    if (ProbeLog()) {
        for (auto& f : g_scanFound) {
            UE_LOGI("water[probe]: key='%ls' idx=%d actor=%p", f.first.c_str(), f.second.idx, f.second.actor);
        }
    }
    g_scanFound.clear();
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "water", nullptr, &WP::EnsureResolved, &WP::IsWaterProp,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 2});
}

enum RefusalCondition : uint32_t {
    Refusal_Envelope   = 1 << 0,  // !isfinite || val < 0 || val > 38
    Refusal_Adopt      = 1 << 1,  // adopt != 0 && adopt != 1
    Refusal_OutOfRange = 1 << 2,  // !WP::IsValidScalar(actor, val)
    Refusal_Apply      = 1 << 3,  // !WP::WriteWaterScalarAndApply(actor, val)
    Refusal_EmptyKey   = 1 << 4,  // StringFromWireKey(payload.key).empty()
};

std::unordered_map<std::wstring, uint32_t> g_refusalLatches;

bool ShouldLogRefusal(const std::wstring& key, uint32_t conditionBit) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    auto& mask = g_refusalLatches[key];
    if (mask & conditionBit) {
        return false;
    }
    mask |= conditionBit;
    return true;
}

bool ApplyResolved(void* actor, const std::wstring& key, float wireValue, bool adopt, unsigned fromSlot) {
    if (!WP::IsValidScalar(actor, wireValue)) {
        if (ShouldLogRefusal(key, Refusal_OutOfRange)) {
            UE_LOGW("water: inbound value %.3f out of range for key='%ls' (from slot %u) -- refusing",
                    wireValue, key.c_str(), fromSlot);
        }
        return false;
    }
    g_echo.store(true, std::memory_order_release);
    const bool ok = WP::WriteWaterScalarAndApply(actor, wireValue);
    g_echo.store(false, std::memory_order_release);
    if (!ok) {
        if (ShouldLogRefusal(key, Refusal_Apply)) {
            UE_LOGW("water: WriteWaterScalarAndApply refused key='%ls' val=%.3f", key.c_str(), wireValue);
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_lastKnown[key] = wireValue;
        g_refusalLatches.erase(key);
    }
    UE_LOGI("water: applied val=%.3f (wire=%.3f adopt=%d) ok=1 key='%ls' (from slot %u)",
            wireValue, wireValue, adopt ? 1 : 0, key.c_str(), fromSlot);
    return true;
}

void PollAndBroadcast() {
    if (g_echo.load(std::memory_order_acquire)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    auto& refs = g_pollScratch;
    refs.clear();
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (g_byKey.empty()) return;
        refs.reserve(g_byKey.size());
        for (auto& kv : g_byKey) refs.emplace_back(kv.first, kv.second);
    }
    for (auto& r : refs) {
        if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) continue;
        float cur = 0.f;
        if (!WP::ReadWaterScalar(r.second.actor, cur)) continue;
        float base = 0.f;
        bool firstSight = false;
        {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            auto it = g_lastKnown.find(r.first);
            if (it == g_lastKnown.end()) {
                g_lastKnown[r.first] = cur;
                firstSight = true;
            } else {
                base = it->second;
            }
        }
        if (firstSight) continue;  // prime silently
        if (std::fabs(cur - base) > kWaterEps) {
            coop::net::WaterStatePayload p{};
            WireKeyFromString(r.first, p.key);
            p.value = cur;
            p.adopt = 0;
            if (s->SendReliable(coop::net::ReliableKind::WaterState, &p, sizeof(p))) {
                std::lock_guard<std::mutex> lk(g_stateMutex);
                g_lastKnown[r.first] = cur;
                UE_LOGI("water: %s val=%.3f key='%ls'",
                        (s->role() == coop::net::Role::Host) ? "host broadcast" : "client request",
                        cur, r.first.c_str());
            } else {
                UE_LOGW("water: SendReliable failed key='%ls'", r.first.c_str());
            }
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (session) {
        g_noSessionLogged.store(false, std::memory_order_release);
    }
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();
}

void OnReliable(const coop::net::WaterStatePayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) {
        // Malformed input: empty wire key cannot be indexed or applied; refuse with throttled log.
        if (ShouldLogRefusal(key, Refusal_EmptyKey)) {
            UE_LOGW("water: OnReliable empty key (slot %u) -- refusing malformed payload", senderPeerSlot);
        }
        return;
    }
    WP::EnsureResolved();
    if (!std::isfinite(payload.value) || payload.value < 0.0f || payload.value > 38.0f) {
        if (ShouldLogRefusal(key, Refusal_Envelope)) {
            UE_LOGW("water: OnReliable invalid payload value=%.3f key='%ls' (slot %u) -- dropping",
                    payload.value, key.c_str(), senderPeerSlot);
        }
        return;
    }
    if (payload.adopt != 0 && payload.adopt != 1) {
        if (ShouldLogRefusal(key, Refusal_Adopt)) {
            UE_LOGW("water: OnReliable invalid adopt=%u key='%ls' (slot %u) -- dropping",
                    static_cast<unsigned>(payload.adopt), key.c_str(), senderPeerSlot);
        }
        return;
    }

    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        // Not in an active session; inbound updates are meaningless without a session context.
        // Throttled per condition (not per key) to avoid log spam and latch table growth.
        if (!g_noSessionLogged.exchange(true, std::memory_order_relaxed)) {
            UE_LOGW("water: OnReliable without active session (slot %u) -- refusing", senderPeerSlot);
        }
        return;
    }

    if (s->role() == coop::net::Role::Host) {
        // HOST ARBITER:
        // Host-slot loopback (senderPeerSlot == 0 on host) is unreachable in normal operation:
        // host receive diverts an unadmitted connection to the admission path (session.cpp:881)
        // and takes the slot from the authenticated connection, ignoring the header's claimed
        // slot (session.cpp:517); host broadcasts do not enqueue locally, because host slot 0
        // holds no connection and SendReliable fans out only over live ones (session.h:656,
        // session.cpp:277). Citations re-verified 2026-08-30 on v148. Harmless defensive
        // guard for an unreachable case; ignore without logging or deferring.
        if (senderPeerSlot == 0) return;

        // Client request received on the host.
        void* actor = ResolveFast(key);
        if (actor) {
            if (!WP::IsValidScalar(actor, payload.value)) {
                if (ShouldLogRefusal(key, Refusal_OutOfRange)) {
                    UE_LOGW("water: host refused out-of-range request val=%.3f key='%ls' from slot %u",
                            payload.value, key.c_str(), senderPeerSlot);
                }
                return;
            }
            if (ApplyResolved(actor, key, payload.value, false, senderPeerSlot)) {
                // Read authoritative post-apply value
                float authVal = payload.value;
                WP::ReadWaterScalar(actor, authVal);

                // Broadcast authoritative value to ALL clients (including origin).
                coop::net::WaterStatePayload outP{};
                WireKeyFromString(key, outP.key);
                outP.value = authVal;
                outP.adopt = 0;
                s->SendReliable(coop::net::ReliableKind::WaterState, &outP, sizeof(outP));
                return;
            }
            // Apply failed (e.g. transient engine issue, dynmat not live) -> do NOT broadcast.
            // Fall through to deferral so it can retry.
        }

        // Defer on host if instance not streamed in yet or apply failed.
        const auto deadline = std::chrono::steady_clock::now() + kPendingTTL;
        auto it = g_pending.find(key);
        if (it == g_pending.end()) {
            if (g_pending.size() >= kMaxPending) {
                LogPendingCapHit(g_pending.size());
                return;
            }
            g_pending[key] = Pending{ payload.value, false, senderPeerSlot, deadline };
        } else {
            it->second.value = payload.value;
            it->second.adopt = false;
            it->second.fromSlot = senderPeerSlot;
            it->second.deadline = deadline;
        }
        if (ProbeLog()) {
            UE_LOGI("water: host deferring request val=%.3f key='%ls' from slot %u",
                    payload.value, key.c_str(), senderPeerSlot);
        }
        return;
    }

    // CLIENT:
    // Accept authoritative host updates (from senderPeerSlot 0). Host-authored snapshot/broadcast
    // rows are not subject to the client-flood cap so joining clients never drop unstreamed props.
    const bool adopt = (payload.adopt != 0);
    if (void* actor = ResolveFast(key)) {
        if (ApplyResolved(actor, key, payload.value, adopt, senderPeerSlot)) {
            return;
        }
        // Apply failed -> fall through to deferral so it can retry when actor/dynmat is ready.
    }
    const auto deadline = std::chrono::steady_clock::now() + kPendingTTL;
    auto it = g_pending.find(key);
    if (it == g_pending.end()) {
        g_pending[key] = Pending{ payload.value, adopt, senderPeerSlot, deadline };
    } else {
        it->second.value = payload.value;
        it->second.adopt = adopt;
        it->second.fromSlot = senderPeerSlot;
        it->second.deadline = deadline;
    }
    if (ProbeLog()) {
        UE_LOGI("water: client deferring '%ls' val=%.3f adopt=%d (slot %u)",
                key.c_str(), payload.value, adopt ? 1 : 0, senderPeerSlot);
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) return;
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    std::vector<std::pair<std::wstring, Ref>> items;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        items.reserve(g_byKey.size());
        for (auto& kv : g_byKey) items.emplace_back(kv.first, kv.second);
    }
    int sent = 0;
    for (auto& d : items) {
        if (!R::IsLiveByIndex(d.second.actor, d.second.idx)) continue;
        float val = 0.f;
        if (!WP::ReadWaterScalar(d.second.actor, val)) continue;
        coop::net::WaterStatePayload p{};
        WireKeyFromString(d.first, p.key);
        p.value = val;
        p.adopt = 1;  // connect-snapshot -> joiner adopts host's world
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WaterState, &p, sizeof(p));
        { std::lock_guard<std::mutex> lk(g_stateMutex); g_lastKnown[d.first] = val; }
        ++sent;
    }
    UE_LOGI("water: connect-snapshot -- sent %d water prop(s) to slot %d (of %zu indexed)",
            sent, peerSlot, items.size());
}

void Tick() {
    const auto now = std::chrono::steady_clock::now();

    // RECEIVER EXPIRY: sweep and expire pending entries on every tick regardless of wrapper
    // readiness or index currency. Distinct keys can never accumulate past kPendingTTL even
    // while BP classes are unresolved or the scan hub index is stale.
    if (!g_pending.empty()) {
        int expired = 0, still = 0;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (now >= it->second.deadline) {
                if (ProbeLog()) {
                    UE_LOGI("water: deferred '%ls' expired (not present on this peer)", it->first.c_str());
                }
                it = g_pending.erase(it);
                ++expired;
            } else {
                ++it;
                ++still;
            }
        }
        if (expired) {
            UE_LOGI("water: retry tick -- applied 0 deferred, dropped %d expired, %d still pending",
                    expired, still);
        }
    }

    if (!WP::EnsureResolved()) return;
    RegisterWithScanHub();
    if (!IndexCurrent()) return;

    auto* s = g_session.load(std::memory_order_acquire);
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        if (!g_pending.empty()) {
            int applied = 0, expired = 0, still = 0;
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                // Check deadline BEFORE attempting an apply -- an expired update must never apply late.
                if (now >= it->second.deadline) {
                    if (ProbeLog()) {
                        UE_LOGI("water: deferred '%ls' expired (not present on this peer)", it->first.c_str());
                    }
                    it = g_pending.erase(it);
                    ++expired;
                    continue;
                }
                if (void* actor = ResolveFast(it->first)) {
                    const bool isHostReq = (s && s->role() == coop::net::Role::Host && it->second.fromSlot != 0);
                    if (ApplyResolved(actor, it->first, it->second.value, it->second.adopt, it->second.fromSlot)) {
                        if (isHostReq) {
                            float authVal = it->second.value;
                            WP::ReadWaterScalar(actor, authVal);
                            coop::net::WaterStatePayload outP{};
                            WireKeyFromString(it->first, outP.key);
                            outP.value = authVal;
                            outP.adopt = 0;
                            s->SendReliable(coop::net::ReliableKind::WaterState, &outP, sizeof(outP));
                        }
                        it = g_pending.erase(it);
                        ++applied;
                        continue;
                    }
                    // Apply failed (e.g. dynmat not live yet) -> do NOT erase; keep pending until deadline.
                }
                ++it;
                ++still;
            }
            if (applied || expired) {
                UE_LOGI("water: retry tick -- applied %d deferred, dropped %d expired, %d still pending",
                        applied, expired, still);
            }
        }
    }
    PollAndBroadcast();
}

void OnDisconnect() {
    g_pending.clear();
    g_lastCapWarn = {};
    g_noSessionLogged.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_stateMutex);
    g_refusalLatches.clear();
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    if (n > 0) UE_LOGI("water: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::water_sync
