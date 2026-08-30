// ue_wrap/devices/water_prop.h -- standalone engine access for VOTV water containers
// and tools: the bucket (prop_bucket_C, prop_bucket_mop_C) and sponge (prop_sponge_C,
// prop_sponge_mega_C, prop_sponge_p_C, prop_sponge_solto_C). Principle-7 engine-wrapper
// layer: wraps reflection, struct offsets, dynamic material parameters, and UFunction
// thunks for bucket and sponge actors. NO network logic, NO gameplay decisions --
// coop::water_sync owns those and talks to the engine through here.
//
// State:
// - Bucket: `height` (float, range 0.0 to 38.0 [standard bucket] or 33.0 [mop bucket]). When height changes,
//   calling prop_bucket_C::upd() pushes the height into the dynamic material `#height` parameter,
//   updates the `empty` boolean, and refreshes the soap/foam visual.
// - Sponge: `power` (float, range 0.0 [dry] to 1.0 [wet]). When power changes, writing
//   dynmat's scalar parameter `#opac` updates the wetness visual.
//
// Identity: Aprop_C::Key FName (inherited from Aprop_C @0x02E0, save-persistent UUID string).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::water_prop {

// Object kind discriminator.
enum class Kind : uint8_t {
    Unknown = 0,
    Bucket,
    Sponge,
};

// Resolve the prop_bucket_C and prop_sponge_C UClasses + property offsets (height,
// power, dynmat) + the prop_bucket_C::upd UFunction + MaterialInstanceDynamic::SetScalarParameterValue.
// Idempotent; returns true once everything resolved (false while BP classes are not loaded
// yet -- caller retries on next tick). FAILS CLOSED if required fields or verbs cannot be resolved.
// Game thread.
bool EnsureResolved();

// Live UClass pointers (nullptr until EnsureResolved succeeds).
void* BucketClass();
void* SpongeClass();
void* MopBucketClass();

// True iff `obj`'s class is prop_bucket_C or a subclass (e.g. prop_bucket_mop_C).
bool IsBucket(void* obj);

// True iff `obj`'s class is prop_bucket_mop_C specifically.
bool IsMopBucket(void* obj);

// True iff `obj`'s class is prop_sponge_C or a subclass (e.g. prop_sponge_mega_C,
// prop_sponge_p_C, prop_sponge_solto_C), explicitly excluding transient splash
// actors (prop_sponge_bucketPour_C).
bool IsSponge(void* obj);

// True iff `obj` is a bucket or sponge.
bool IsWaterProp(void* obj);

// Classify `obj`'s kind (Bucket, Sponge, or Unknown).
Kind Classify(void* obj);

// Read the prop's Aprop_C::Key FName as a wide string. Empty on failure (null / not resolved).
std::wstring GetKeyString(void* prop);

// Maximum allowed scalar for `prop` (38.0 for bucket, 33.0 for mop bucket, 1.0 for sponge; 0.0 on unknown).
float MaxScalar(void* prop);

// Returns true iff `value` is finite and within the exact valid range [0.0, MaxScalar(prop)] for this prop.
bool IsValidScalar(void* prop, float value);

// Read the bucket's `height` scalar (0.0 to 38.0 / 33.0) into `out`. Returns false on failure.
bool ReadBucketHeight(void* bucket, float& out);

// Write the bucket's `height` scalar + dispatch prop_bucket_C::upd() to repaint dynamic
// material `#height`, refresh `empty`, and update soap visual. Fails closed and refuses
// if out-of-range, null, or unresolved.
bool WriteBucketHeightAndApply(void* bucket, float height);

// Read the sponge's `power` scalar (0.0 to 1.0) into `out`. Returns false on failure.
bool ReadSpongePower(void* sponge, float& out);

// Write the sponge's `power` scalar + update dynmat's `#opac` parameter. Fails closed
// and refuses if out-of-range, null, or unresolved.
bool WriteSpongePowerAndApply(void* sponge, float power);

// Generic scalar read/write for any water prop (bucket or sponge). Validates range and fails closed.
bool ReadWaterScalar(void* prop, float& out);
bool WriteWaterScalarAndApply(void* prop, float value);

}  // namespace ue_wrap::water_prop
