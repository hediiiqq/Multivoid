// ue_wrap/devices/water_prop.cpp -- see ue_wrap/devices/water_prop.h. Engine access
// for VOTV water containers and tools (prop_bucket_C and prop_sponge_C).
//
// Offsets are resolved from the live classes via reflection (FindPropertyOffset) with
// the documented Alpha 0.9.0-n CXX-dump values as logged fallbacks (version-tagging rule).
// Fails closed if essential classes, fields, or verbs do not resolve.

#include "ue_wrap/devices/water_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace ue_wrap::water_prop {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_bucketCls          = nullptr;  // prop_bucket_C UClass
void*   g_mopBucketCls       = nullptr;  // prop_bucket_mop_C UClass
void*   g_spongeCls          = nullptr;  // prop_sponge_C UClass
void*   g_spongePourCls      = nullptr;  // prop_sponge_bucketPour_C UClass (excluded)
int32_t g_bucketHeightOff    = -1;       // prop_bucket_C::height  (Alpha 0.9.0-n: 0x0380)
void*   g_bucketUpdFn        = nullptr;  // prop_bucket_C::upd()   (shader + empty + soap refresh)
int32_t g_spongePowerOff     = -1;       // prop_sponge_C::power   (Alpha 0.9.0-n: 0x02F8)
int32_t g_spongeDynmatOff    = -1;       // prop_sponge_C::dynmat  (Alpha 0.9.0-n: 0x0300)
void*   g_dynmatSetScalarFn  = nullptr;  // UMaterialInstanceDynamic::SetScalarParameterValue

// Documented Alpha 0.9.0-n reference values (CXXHeaderDump / kismet disassembly).
// Note: essential properties/verbs MUST resolve via reflection; failure to resolve
// fails closed rather than silently falling back to a hardcoded offset.
constexpr int32_t kBucketHeightOffReference = 0x0380;
constexpr int32_t kSpongePowerOffReference  = 0x02F8;
constexpr int32_t kSpongeDynmatOffReference = 0x0300;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* bucketCls = R::FindClass(L"prop_bucket_C");
    void* spongeCls = R::FindClass(L"prop_sponge_C");
    if (!bucketCls || !spongeCls) {
        return false;  // BP classes not loaded yet -- caller retries
    }

    void* mopBucketCls = R::FindClass(L"prop_bucket_mop_C");

    // Optional splash actor class for explicit exclusion in IsSponge (also excluded by name).
    void* spongePourCls = R::FindClass(L"prop_sponge_bucketPour_C");

    int32_t bucketHeightOff = R::FindPropertyOffset(bucketCls, L"height");
    if (bucketHeightOff < 0) {
        UE_LOGE("water_prop: reflected bucket height property not found -- failing closed");
        return false;
    }

    void* bucketUpdFn = R::FindFunction(bucketCls, L"upd");
    if (!bucketUpdFn) {
        UE_LOGE("water_prop: bucket upd UFunction not found -- failing closed");
        return false;
    }

    int32_t spongePowerOff = R::FindPropertyOffset(spongeCls, L"power");
    if (spongePowerOff < 0) {
        UE_LOGE("water_prop: reflected sponge power property not found -- failing closed");
        return false;
    }

    int32_t spongeDynmatOff = R::FindPropertyOffset(spongeCls, L"dynmat");
    if (spongeDynmatOff < 0) {
        UE_LOGE("water_prop: reflected sponge dynmat property not found -- failing closed");
        return false;
    }

    void* dynmatSetScalarFn = nullptr;
    if (void* mc = R::FindClass(L"MaterialInstanceDynamic")) {
        dynmatSetScalarFn = R::FindFunction(mc, L"SetScalarParameterValue");
    }
    if (!dynmatSetScalarFn) {
        UE_LOGE("water_prop: MaterialInstanceDynamic SetScalarParameterValue UFunction not found -- failing closed");
        return false;
    }

    g_bucketCls          = bucketCls;
    g_mopBucketCls       = mopBucketCls;
    g_spongeCls          = spongeCls;
    g_spongePourCls      = spongePourCls;
    g_bucketHeightOff    = bucketHeightOff;
    g_bucketUpdFn        = bucketUpdFn;
    g_spongePowerOff     = spongePowerOff;
    g_spongeDynmatOff    = spongeDynmatOff;
    g_dynmatSetScalarFn  = dynmatSetScalarFn;

    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("water_prop: resolved bucket_C=%p (height@0x%04X, upd=%p) sponge_C=%p (power@0x%04X, dynmat@0x%04X, setScalar=%p)",
            bucketCls, bucketHeightOff, bucketUpdFn, spongeCls, spongePowerOff, spongeDynmatOff, dynmatSetScalarFn);
    return true;
}

void* BucketClass() { return g_bucketCls; }
void* SpongeClass() { return g_spongeCls; }
void* MopBucketClass() { return g_mopBucketCls; }

bool IsBucket(void* obj) {
    if (!obj || !g_bucketCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_bucketCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool IsMopBucket(void* obj) {
    if (!obj) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    if (g_mopBucketCls && cls == g_mopBucketCls) return true;
    return R::NameContains(R::NameOf(cls), L"prop_bucket_mop");
}

bool IsSponge(void* obj) {
    if (!obj || !g_spongeCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    if (g_spongePourCls && cls == g_spongePourCls) return false;  // exclude splash actor
    const auto& clsName = R::NameOf(cls);
    if (R::NameContains(clsName, L"bucketPour") || R::NameContains(clsName, L"sponge_bucketPour")) {
        return false;  // load-order-independent splash exclusion
    }
    void* bases[1] = { g_spongeCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool IsWaterProp(void* obj) {
    return IsBucket(obj) || IsSponge(obj);
}

Kind Classify(void* obj) {
    if (IsBucket(obj)) return Kind::Bucket;
    if (IsSponge(obj)) return Kind::Sponge;
    return Kind::Unknown;
}

float MaxScalar(void* prop) {
    if (!prop) return 0.0f;
    if (IsMopBucket(prop)) {
        return 33.0f;
    }
    if (IsBucket(prop)) {
        return 38.0f;
    }
    if (IsSponge(prop)) {
        return 1.0f;
    }
    return 0.0f;
}

bool IsValidScalar(void* prop, float value) {
    if (!prop || !std::isfinite(value) || value < 0.0f) return false;
    const float maxVal = MaxScalar(prop);
    if (maxVal <= 0.0f) return false;
    return value <= maxVal;
}

std::wstring GetKeyString(void* prop) {
    if (!prop) return std::wstring();
    std::wstring key = ue_wrap::prop::GetKeyString(prop);
    if (key.empty() || key == L"None") {
        key = ue_wrap::prop::GetInteractableKeyString(prop);
    }
    return key;
}

bool ReadBucketHeight(void* bucket, float& out) {
    if (!bucket || g_bucketHeightOff < 0) return false;
    out = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(bucket) + g_bucketHeightOff);
    return true;
}

bool WriteBucketHeightAndApply(void* bucket, float height) {
    if (!bucket || !g_resolved.load(std::memory_order_acquire) || g_bucketHeightOff < 0 || !g_bucketUpdFn) {
        return false;
    }
    if (!IsValidScalar(bucket, height)) {
        return false;
    }
    // Validate parameter frame BEFORE writing memory.
    ParamFrame f(g_bucketUpdFn);
    if (!f.valid()) {
        return false;
    }
    // upd() has zero parameters in BP -- reads this->height and updates dynamic material
    // parameter #height, sets empty bool, and adjusts soap color/visibility.
    const float oldHeight = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(bucket) + g_bucketHeightOff);
    *reinterpret_cast<float*>(reinterpret_cast<char*>(bucket) + g_bucketHeightOff) = height;
    if (!Call(bucket, f)) {
        // Restore prior scalar on dispatch failure so memory is never left in half-applied state.
        *reinterpret_cast<float*>(reinterpret_cast<char*>(bucket) + g_bucketHeightOff) = oldHeight;
        return false;
    }
    return true;
}

bool ReadSpongePower(void* sponge, float& out) {
    if (!sponge || g_spongePowerOff < 0) return false;
    out = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(sponge) + g_spongePowerOff);
    return true;
}

bool WriteSpongePowerAndApply(void* sponge, float power) {
    if (!sponge || !g_resolved.load(std::memory_order_acquire) || g_spongePowerOff < 0 || g_spongeDynmatOff < 0 || !g_dynmatSetScalarFn) {
        return false;
    }
    if (!IsValidScalar(sponge, power)) {
        return false;
    }
    void* dynmat = *reinterpret_cast<void**>(reinterpret_cast<char*>(sponge) + g_spongeDynmatOff);
    if (!dynmat || !R::IsLive(dynmat)) {
        return false;
    }
    // Validate frame, set every parameter, and execute dynamic material call BEFORE writing power.
    ParamFrame f(g_dynmatSetScalarFn);
    if (!f.valid()) {
        return false;
    }
    const R::FName paramName = ue_wrap::fname_utils::StringToFName(L"opac");
    if (!f.Set(L"ParameterName", paramName)) {
        return false;
    }
    if (!f.Set(L"Value", power)) {
        return false;
    }
    if (!Call(dynmat, f)) {
        return false;
    }
    // Material parameter updated successfully -- write scalar to sponge memory.
    *reinterpret_cast<float*>(reinterpret_cast<char*>(sponge) + g_spongePowerOff) = power;
    return true;
}

bool ReadWaterScalar(void* prop, float& out) {
    switch (Classify(prop)) {
    case Kind::Bucket:
        return ReadBucketHeight(prop, out);
    case Kind::Sponge:
        return ReadSpongePower(prop, out);
    default:
        return false;
    }
}

bool WriteWaterScalarAndApply(void* prop, float value) {
    switch (Classify(prop)) {
    case Kind::Bucket:
        return WriteBucketHeightAndApply(prop, value);
    case Kind::Sponge:
        return WriteSpongePowerAndApply(prop, value);
    default:
        return false;
    }
}

}  // namespace ue_wrap::water_prop
