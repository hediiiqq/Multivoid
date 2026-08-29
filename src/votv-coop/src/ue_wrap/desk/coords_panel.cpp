// ue_wrap/desk/coords_panel.cpp -- see ue_wrap/desk/coords_panel.h. Bodies
// verbatim from the console_desk.cpp originals (2026-07-19 one-class-per-file
// split); the desk half of the instance chain stays in console_desk
// (AtlasUiCoordsSlot), the desk's updateCoordCoords verb is reached through
// console_desk::CallUpdateCoordCoords.

#include "ue_wrap/desk/coords_panel.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::coords_panel {
namespace {

namespace R = ue_wrap::reflection;

// ui_coordinates (the coords-panel widget; SINGLETON child of the atlas) --
// the REAL dish-aim state + its repaint verb.
void* g_uiCoordsCls = nullptr;
void* g_uiCoordsInst = nullptr;
int32_t g_uiCoordsIdx = -1;
int32_t g_offViewCoordinate = -1;  // FVector2D @0x03B8
int32_t g_offCoordinate0 = -1;     // FVector2D @0x03C0
int32_t g_offCoordinate1 = -1;     // FVector2D @0x03C8
int32_t g_offCoordinate2 = -1;     // FVector2D @0x03D0
int32_t g_offSelected = -1;        // @0x03D8
int32_t g_offDirection = -1;       // bool @0x0441 -- the catch-gate toggle (v70)
void* g_updCursorLocationsFn = nullptr;

// The REQUIRED set (cls + the five aim offsets + the repaint verb) resolved --
// the steady-state fast gate. Direction is OPTIONAL at the read site (as in
// the pre-split code) but resolves in the same pass from the same class dump.
bool g_required = false;
std::chrono::steady_clock::time_point g_nextResolve{};

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);

    if (!g_uiCoordsCls) g_uiCoordsCls = R::FindClass(L"ui_coordinates_C");
    if (g_uiCoordsCls) {
        if (g_offViewCoordinate < 0)
            g_offViewCoordinate = R::FindPropertyOffset(g_uiCoordsCls, L"viewCoordinate");
        if (g_offCoordinate0 < 0)
            g_offCoordinate0 = R::FindPropertyOffset(g_uiCoordsCls, L"Coordinate_0");
        if (g_offCoordinate1 < 0)
            g_offCoordinate1 = R::FindPropertyOffset(g_uiCoordsCls, L"Coordinate_1");
        if (g_offCoordinate2 < 0)
            g_offCoordinate2 = R::FindPropertyOffset(g_uiCoordsCls, L"Coordinate_2");
        if (g_offSelected < 0)
            g_offSelected = R::FindPropertyOffset(g_uiCoordsCls, L"selected");
        if (g_offDirection < 0)
            g_offDirection = R::FindPropertyOffset(g_uiCoordsCls, L"Direction");
        if (!g_updCursorLocationsFn)
            g_updCursorLocationsFn = R::FindFunction(g_uiCoordsCls, L"updCursorLocations");
    }

    if (!g_required && g_uiCoordsCls && g_updCursorLocationsFn &&
        g_offViewCoordinate >= 0 && g_offCoordinate0 >= 0 && g_offCoordinate1 >= 0 &&
        g_offCoordinate2 >= 0 && g_offSelected >= 0) {
        g_required = true;
        UE_LOGI("coords_panel: resolved -- view/c0/sel/dir=0x%X/0x%X/0x%X/0x%X updCursor=%s",
                g_offViewCoordinate, g_offCoordinate0, g_offSelected, g_offDirection,
                g_updCursorLocationsFn ? "yes" : "NO");
    }
}

}  // namespace

void* Instance() {
    if (g_uiCoordsInst && R::IsLiveByIndex(g_uiCoordsInst, g_uiCoordsIdx)) return g_uiCoordsInst;
    g_uiCoordsInst = nullptr;
    if (!g_required) ResolvePass();
    if (!g_uiCoordsCls) return nullptr;
    // Resolve via the desk's OWN field chain (desk.Widget -> atlas.ui_coordinates)
    // -- the game's access path (getCoordWidget does exactly this). The previous
    // FindObjectsByClass(ui_coordinates_C) first-hit grabbed the cooked
    // widget-tree TEMPLATE (loads with the class, IsLive, NOT Default__-named,
    // permanently resident -> lowest index wins) and cached it forever: the
    // host wrote dot state into the template (invisible) and the client read
    // the template's frozen fields (nothing to stream) = the dead star-map
    // mirror (STOLAS RE 2026-06-12 root cause 1).
    void* coords = ue_wrap::console_desk::AtlasUiCoordsSlot();
    if (!coords || !R::IsLive(coords)) return nullptr;
    void* cls = R::ClassOf(coords);
    if (!cls || !R::IsDescendantOfAny(cls, &g_uiCoordsCls, 1)) {
        UE_LOGW("coords_panel: desk.Widget.ui_coordinates is not a ui_coordinates_C (cls=%p) -- not caching",
                cls);
        return nullptr;
    }
    g_uiCoordsInst = coords;
    g_uiCoordsIdx = R::InternalIndexOf(coords);
    UE_LOGI("coords_panel: ui_coordinates LIVE instance resolved via desk.Widget chain (%p)", coords);
    return g_uiCoordsInst;
}

bool ReadDishAim(DishAim& out) {
    if (!g_required) ResolvePass();
    void* w = Instance();
    if (!w || g_offViewCoordinate < 0 || g_offCoordinate0 < 0 ||
        g_offCoordinate1 < 0 || g_offCoordinate2 < 0 || g_offSelected < 0)
        return false;
    auto* p = reinterpret_cast<uint8_t*>(w);
    auto rd = [&](int32_t off, float& x, float& y) {
        std::memcpy(&x, p + off + 0, sizeof(float));
        std::memcpy(&y, p + off + 4, sizeof(float));
    };
    rd(g_offViewCoordinate, out.viewX, out.viewY);
    rd(g_offCoordinate0, out.c0X, out.c0Y);
    rd(g_offCoordinate1, out.c1X, out.c1Y);
    rd(g_offCoordinate2, out.c2X, out.c2Y);
    std::memcpy(&out.selected, p + g_offSelected, sizeof(int32_t));
    out.direction = (g_offDirection >= 0 && *(p + g_offDirection)) ? 1 : 0;
    return true;
}

// v109: the LIVE cursor apply (the DeskCursorPose stream, ~60Hz interpolated).
// INVARIANT -- writes ONLY viewCoordinate and dispatches NOTHING. It must NEVER
// call updCursorLocations: that is the committed-triangle + pingDishes setRot loop
// (N UFunction dispatches), and running it at 60Hz is a per-frame dispatch STORM.
// The widget's OWN Tick repaints the cursor from viewCoordinate (updMainCursor) --
// keeping the field fresh IS the whole job here. If you need the triangle/dish
// repaint, that is WriteDishCommitted (commit rate), NOT this. The name is the
// guard: WriteCursorOnly does cursor, only.
bool WriteCursorOnly(float viewX, float viewY) {
    if (!g_required) ResolvePass();
    void* w = Instance();
    if (!w || g_offViewCoordinate < 0) return false;
    auto* p = reinterpret_cast<uint8_t*>(w);
    std::memcpy(p + g_offViewCoordinate + 0, &viewX, sizeof(float));
    std::memcpy(p + g_offViewCoordinate + 4, &viewY, sizeof(float));
    return true;  // NO UFunction dispatch -- see the INVARIANT above.
}

// v109: the COMMITTED-coords apply (the discrete triangulation locks). Writes
// Coordinate_0/1/2 + selected + direction and calls updCursorLocations (repaints
// the committed triangle + rotates the physical pingDishes) + updateCoordCoords
// (az/alt text). Does NOT touch viewCoordinate -- the live cursor is owned by the
// DeskCursorPose stream (WriteCursorOnly). Runs at COMMIT rate (change-gated), so
// the per-call updCursorLocations dish-setRot loop is fine here (it is NOT per-frame).
bool WriteDishCommitted(const DishAim& in) {
    if (!g_required) ResolvePass();
    void* w = Instance();
    if (!w || g_offCoordinate0 < 0 || g_offCoordinate1 < 0 ||
        g_offCoordinate2 < 0 || g_offSelected < 0)
        return false;
    auto* p = reinterpret_cast<uint8_t*>(w);
    auto wr = [&](int32_t off, float x, float y) {
        std::memcpy(p + off + 0, &x, sizeof(float));
        std::memcpy(p + off + 4, &y, sizeof(float));
    };
    wr(g_offCoordinate0, in.c0X, in.c0Y);
    wr(g_offCoordinate1, in.c1X, in.c1Y);
    wr(g_offCoordinate2, in.c2X, in.c2Y);
    std::memcpy(p + g_offSelected, &in.selected, sizeof(int32_t));
    if (g_offDirection >= 0) *(p + g_offDirection) = in.direction ? 1 : 0;
    if (g_updCursorLocationsFn) {
        ue_wrap::ParamFrame f(g_updCursorLocationsFn);
        if (f.valid()) ue_wrap::Call(w, f);
    }
    // The desk's az/alt text repaint (updateCoordCoords) -- the desk owns its
    // verb table; declines silently on an unresolved desk, as pre-split.
    ue_wrap::console_desk::CallUpdateCoordCoords();
    return true;
}

}  // namespace ue_wrap::coords_panel
