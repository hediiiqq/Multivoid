// ue_wrap/devices/cremator.cpp -- see ue_wrap/devices/cremator.h. Engine access for the cremator
// (Acremator_C). Offsets/verbs resolved from the live class via reflection (version-portable);
// the Alpha 0.9.0-n values are logged fallbacks. Fails closed if apply verbs are absent.

#include "ue_wrap/devices/cremator.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::cremator {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};
std::atomic<bool> g_keyResolved{false};

void*   g_cls                = nullptr;  // cremator_C UClass
int32_t g_keyOff             = -1;       // Aactor_save_C::Key (Alpha 0.9.0-n: 0x0230)
int32_t g_isClosedOff        = -1;       // Acremator_C::isClosed (Alpha 0.9.0-n: 0x0399)
int32_t g_animOff            = -1;       // Acremator_C::anim
int32_t g_isActiveOff        = -1;       // Acremator_C::isActive
int32_t g_lookingAtHandleOff = -1;       // Acremator_C::lookingAt_handle
void*   g_useLeverFn         = nullptr;  // useLever() -- triggers the native lever pull, door close, ignite
void*   g_setClosedFn        = nullptr;  // setClosed(bool close) -- door close/lock setter

constexpr int32_t kKeyOffFallback      = 0x0230;
constexpr int32_t kIsClosedOffFallback = 0x0399;

}  // namespace

bool EnsureResolved() {
    // The shared Key lives on the Aactor_save_C base; FindPropertyOffset does NOT climb to a
    // super, so resolve it against actor_save_C directly (same gotcha garage/appliance handle).
    if (!g_keyResolved.load(std::memory_order_acquire)) {
        void* saveCls = R::FindClass(L"actor_save_C");
        if (!saveCls) return false;  // base not loaded yet
        int32_t k = R::FindPropertyOffset(saveCls, L"Key");
        if (k < 0) {
            UE_LOGW("cremator: reflected Key offset not found -- using fallback 0x0230");
            k = kKeyOffFallback;
        }
        g_keyOff = k;
        g_keyResolved.store(true, std::memory_order_release);
        UE_LOGI("cremator: Key@0x%04X (actor_save_C)", k);
    }

    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"cremator_C");
    if (!cls) return false;

    int32_t isClosedOff = R::FindPropertyOffset(cls, L"isClosed");
    if (isClosedOff < 0) {
        UE_LOGW("cremator: isClosed offset not found -- fallback 0x%04X", kIsClosedOffFallback);
        isClosedOff = kIsClosedOffFallback;
    }

    int32_t animOff = R::FindPropertyOffset(cls, L"anim");
    if (animOff < 0) {
        UE_LOGW("cremator: anim offset not found");
    }

    int32_t isActiveOff = R::FindPropertyOffset(cls, L"isActive");
    if (isActiveOff < 0) {
        UE_LOGW("cremator: isActive offset not found");
    }

    int32_t lookingAtHandleOff = R::FindPropertyOffset(cls, L"lookingAt_handle");
    if (lookingAtHandleOff < 0) {
        UE_LOGW("cremator: lookingAt_handle offset not found");
    }

    void* useLeverFn = R::FindFunction(cls, L"useLever");
    if (!useLeverFn) {
        UE_LOGW("cremator: useLever() apply verb not found -- resolution refused (fail-closed)");
        return false;
    }

    void* setClosedFn = R::FindFunction(cls, L"setClosed");
    if (!setClosedFn) {
        UE_LOGW("cremator: setClosed() verb not found -- resolution refused (fail-closed)");
        return false;
    }

    g_cls                = cls;
    g_isClosedOff        = isClosedOff;
    g_animOff            = animOff;
    g_isActiveOff        = isActiveOff;
    g_lookingAtHandleOff = lookingAtHandleOff;
    g_useLeverFn         = useLeverFn;
    g_setClosedFn        = setClosedFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("cremator: resolved cremator_C=%p isClosed@0x%04X anim@0x%04X isActive@0x%04X lookingAt_handle@0x%04X useLever=%p setClosed=%p",
            cls, isClosedOff, animOff, isActiveOff, lookingAtHandleOff, useLeverFn, setClosedFn);
    return true;
}

bool IsCremator(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    if (cls == g_cls) return true;
    void* bases[1] = { g_cls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* a) {
    if (!a || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(a) + g_keyOff);
    return R::ToString(key);
}

bool TryReadState(void* a, bool& on) {
    if (!a || g_isClosedOff < 0 || !IsCremator(a)) return false;
    on = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(a) + g_isClosedOff);
    return true;
}

bool ApplyState(void* a, bool on) {
    if (!a || !IsCremator(a)) return false;
    if (on) {
        if (!g_useLeverFn) return false;
        ParamFrame f(g_useLeverFn);
        if (!f.valid()) return false;
        return Call(a, f);
    } else {
        if (!g_setClosedFn) return false;
        ParamFrame f(g_setClosedFn);
        if (!f.valid()) return false;
        f.Set<bool>(L"close", false);
        return Call(a, f);
    }
}

bool IsReady(void* a) {
    if (!a || !IsCremator(a)) return false;
    if (g_isClosedOff >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(a) + g_isClosedOff))
        return false;
    if (g_isActiveOff >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(a) + g_isActiveOff))
        return false;
    if (g_animOff >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(a) + g_animOff))
        return false;
    return true;
}

bool GetAnim(void* a) {
    if (!a || g_animOff < 0 || !IsCremator(a)) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(a) + g_animOff);
}

void SetAnim(void* a, bool anim) {
    if (!a || g_animOff < 0 || !IsCremator(a)) return;
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(a) + g_animOff) = anim;
}

bool IsLookingAtHandle(void* a) {
    if (!a || !IsCremator(a)) return false;
    if (g_lookingAtHandleOff >= 0)
        return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(a) + g_lookingAtHandleOff);
    return true;  // if lookingAt_handle reflection missing, allow fallback to true
}

}  // namespace ue_wrap::cremator
