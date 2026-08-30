// ue_wrap/garage.cpp -- see ue_wrap/garage.h. Engine access for the base garage door
// (Agarage_C). Offsets resolved from the live class via reflection (version-portable);
// the Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/devices/garage.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ue_wrap::garage {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_garageCls = nullptr;  // garage_C UClass
int32_t g_openOff   = -1;       // Agarage_C::Open     (0x02E8)
void*   g_acivaeFn  = nullptr;  // acivae() -- the NATIVE animated swing (montage from 0 @0.5x + the
                                // move timeline over its full duration). NOT settime: settime SNAPS
                                // (move.SetNewTime(endpoint) + montage @StartingPosition=100) = the
                                // "too fast" the user saw. Bytecode-verified 2026-06-09.
// NOTE: no Key offset -- identity is the level-export FName (GetNameKey), not the save Key
// (RULE 2: the AtriggerBase_C::Key resolution was retired with the R9 fix, see garage.h).

constexpr int32_t kOpenOffFallback = 0x02E8;

// TEMPORARY DIAGNOSTIC (fork-local, 2026-08-31) -- upstream issue #11.
// A client that joins and loads the host's save world ends up with an EMPTY garage index
// while the actor is demonstrably there (its own door opens locally). Two candidates could
// not be separated from the logs: the latched UClass going stale across that world load, or
// something else stopping IsGarage from matching. This answers it directly: every 10 s it
// checks that the latched class object is still LIVE, then counts live placed actors two ways
// -- by the class's FName (a replacement class of the same name shares it, since UE4.27 interns
// names per string) and by the pointer descendant test IsGarage actually uses. If the name count is nonzero while the pointer count is zero, the
// latch is the defect and nothing else needs guessing.
// Costs one GUObjectArray pass per 10 s and stops logging after kDiagMaxLines lines.
// DELETE once the question is answered.
void GarageClassDiag() {
    using namespace std::chrono;
    static steady_clock::time_point sNext{};
    static int sLines = 0;
    constexpr int kDiagMaxLines = 40;
    if (sLines >= kDiagMaxLines) return;
    const auto now = steady_clock::now();
    if (now < sNext) return;
    sNext = now + seconds(10);

    // Count by CLASS NAME, not by class pointer -- and compare the FName's comparison index
    // rather than rendering a string, so the walk stays a couple of integer compares per object.
    //
    // Why not FindClass: it is CACHED (reflection.cpp:494) and returns the cached UClass while
    // that object is still live and still named garage_C. It never looks for a NEWER class of the
    // same name. So a pointer census against it would answer the same stale question the latch
    // already asks, and "SAME" would prove nothing. The name index is immune to that: it matches
    // the old class object and a freshly created one alike, which is exactly the case we are here
    // to detect.
    // The latched pointer is the very thing under suspicion, so validate it BEFORE reading through
    // it: NameOf is an unguarded read at +0x18 (reflection.cpp:288). A purged class would fault
    // here, and a recycled address would hand us an unrelated FName and worthless counts. IsLive
    // checks the object is still published and not pending-kill, so a false answer here IS the
    // finding -- report it and stop, rather than walking the array with a dead target.
    if (!g_garageCls || !R::IsLive(g_garageCls)) {
        ++sLines;
        UE_LOGW("garage[diag]: latched class object %p is NOT LIVE -- the resolve latch is stale "
                "(EnsureResolved never re-resolves; garage.cpp:35). This alone explains an empty "
                "garage index.", g_garageCls);
        return;
    }
    const R::FName targetName = R::NameOf(g_garageCls);
    const int32_t n = R::NumObjects();
    int byClassName = 0, byIsGarage = 0;
    void*   distinctCls[3] = {nullptr, nullptr, nullptr};  // how many DIFFERENT garage_C classes are live
    int     distinctCount = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        void* cls = R::ClassOf(o);
        if (!cls) continue;
        const R::FName cn = R::NameOf(cls);
        const bool nameHit = (cn.ComparisonIndex == targetName.ComparisonIndex &&
                              cn.Number == targetName.Number);
        const bool isLatched = IsGarage(o);
        if (!nameHit && !isLatched) continue;
        // Skip the class default object: it is not a placed actor, and counting it turns an
        // empty world into a misleading 1/1.
        if (R::ToString(R::NameOf(o)).rfind(L"Default__", 0) == 0) continue;
        if (nameHit)   ++byClassName;
        if (isLatched) ++byIsGarage;
        if (nameHit && distinctCount < 3) {
            bool seen = false;
            for (int k = 0; k < distinctCount; ++k) if (distinctCls[k] == cls) { seen = true; break; }
            if (!seen) distinctCls[distinctCount++] = cls;
        }
    }
    ++sLines;
    UE_LOGW("garage[diag]: latched=%p | live placed actors: byClassName=%d byIsGarage=%d | "
            "distinct garage_C classes in use: %d (%p, %p, %p) | objects scanned %d",
            g_garageCls, byClassName, byIsGarage, distinctCount,
            distinctCls[0], distinctCls[1], distinctCls[2], n);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) {
        GarageClassDiag();  // TEMPORARY -- see above; delete with it.
        return true;
    }

    void* cls = R::FindClass(L"garage_C");
    if (!cls) return false;

    int32_t openOff = R::FindPropertyOffset(cls, L"Open");
    if (openOff < 0) {
        UE_LOGW("garage: reflected Open offset not found -- using fallback 0x%04X", kOpenOffFallback);
        openOff = kOpenOffFallback;
    }
    void* acivaeFn = R::FindFunction(cls, L"acivae");
    if (!acivaeFn) {
        UE_LOGW("garage: acivae UFunction not found -- not ready");
        return false;
    }

    g_garageCls = cls;
    g_openOff   = openOff;
    g_acivaeFn  = acivaeFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("garage: resolved garage_C=%p Open@0x%04X acivae=%p (identity=level-export FName)",
            cls, openOff, acivaeFn);
    return true;
}

bool IsGarage(void* obj) {
    if (!obj || !g_garageCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_garageCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetNameKey(void* g) {
    // Identity = the garage's level-export FName (baked into the cooked package -> deterministic +
    // cross-peer stable), NOT the save Key. Mirrors ue_wrap::door_box::GetNameKey (the proven author
    // for keyless placed actors). See garage.h for why the save Key is unreliable (R9).
    if (!g) return std::wstring();
    return R::ToString(R::NameOf(g));
}

bool TryReadOpen(void* g, bool& open) {
    if (!g || g_openOff < 0) return false;
    open = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(g) + g_openOff);
    return true;
}

bool ApplyOpen(void* g, bool open) {
    if (!g || !g_acivaeFn) return false;
    // Idempotent: if already in the target state, do nothing (skip the re-trigger + the echo).
    bool cur = false;
    if (TryReadOpen(g, cur) && cur == open) return true;
    // Two bytecode-verified facts (RE 2026-06-09) drive this:
    //  (1) Neither settime() NOR acivae() writes the `Open` bool @0x02E8 -- the ONLY writers are
    //      runTrigger's E-press toggle and the game's own loadTriggerData (`open := value; settime`).
    //      So we MUST set the field ourselves, else the mirror's poll baseline goes stale and the
    //      symmetric Channel re-broadcasts the opposite -> the open/close OSCILLATION the user first saw.
    //  (2) settime() SNAPS (move.SetNewTime(endpoint) + a montage at StartingPosition=100) -> the
    //      "garage slides too fast on the host" the user saw next; acivae() ANIMATES (montage from 0
    //      @0.5x + move.Play/Reverse over the full ~10s, DIRECTION read from the `Open` field).
    // So: write Open := target FIRST (fixes the oscillation + gives acivae its direction), THEN call
    // acivae() for the NATIVE animated swing (fixes the too-fast). acivae has no `mov` guard, so a
    // mid-swing opposite packet just re-aims it (last-writer-wins). This is exactly the code path a
    // local E-press takes (runTrigger toggles Open -> acivae), minus the toggle.
    if (g_openOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;
    ParamFrame f(g_acivaeFn);  // acivae() takes no params -- it reads the Open field for direction
    if (!f.valid()) return false;
    return Call(g, f);
}

}  // namespace ue_wrap::garage
