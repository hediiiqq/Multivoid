// ue_wrap/devices/cremator.h -- standalone engine access for VOTV's cremator (Acremator_C).
// Principle-7 engine-wrapper layer (no network/coop state). coop::interactable_sync drives
// the sync through here via g_applianceAdapter.
//
// Acremator_C inherits Aactor_save_C, carrying a persistent save Key @0x0230. State is the
// isClosed bool (@0x0399) indicating the cremator is closed/operating, driven by the
// native useLever() (or setClosed) verb so the handle animates, the door locks, and the
// burn sequence / sounds run on the peer.
//
// RE: research/findings/props-lifecycle/votv-all-interactables-sweep-catalog-2026-06-08.md:173.

#pragma once

#include <string>

namespace ue_wrap::cremator {

// Resolve the shared Aactor_save_C::Key offset + cremator_C UClass / bool offsets /
// apply verbs (useLever, setClosed). Returns true once Key offset, cremator_C class,
// and all required verbs are resolved (fails closed if any verb is missing). Idempotent. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is (a descendant of) cremator_C. Cheap (pointer compare +
// hierarchy walk); false until resolved.
bool IsCremator(void* obj);

// The cremator's Aactor_save_C::Key as a wide string ("" on failure, L"None" if unkeyed).
std::wstring GetKeyString(void* a);

// Read the cremator's operating/closed state bool into `on`. False if the read could not
// be made (null / not cremator / not resolved); leaves `on` untouched on failure.
bool TryReadState(void* a, bool& on);

// Drive the cremator to `on`: on=true calls useLever() to run the native machine sequence
// (handle animation, door close+lock, sound, particle/fireball FX, burn cycle); on=false
// calls setClosed(false) which unlocks the child swinger door (swingerDoor.locked=false) and
// clears isClosed without forcing a physical open. Leaving the door shut is acceptable (and
// preferred over openDoor) because the door is an independent swinger prop whose unlocked
// resting pose can be pushed/pulled by the player, and forcing open on an OFF connect snapshot
// would unnaturally pop open an idle machine. MUST run on the game thread. False on null / unresolved.
bool ApplyState(void* a, bool on);

// Check if the cremator is idle and ready to accept an activation request (not closed,
// not actively burning, not in the middle of lever animation). Game thread.
bool IsReady(void* a);

// Read the `anim` flag (true while lever timeline/animation is in flight).
bool GetAnim(void* a);

// Write the `anim` flag. Used by client E-press PRE-observer to suppress local BP lever execution.
void SetAnim(void* a, bool anim);

// Check if the player interaction trace is currently aiming at the cremator handle.
bool IsLookingAtHandle(void* a);

}  // namespace ue_wrap::cremator
