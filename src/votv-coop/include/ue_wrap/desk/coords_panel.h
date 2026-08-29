// ue_wrap/desk/coords_panel.h -- standalone engine access for the coords-panel
// widget (Uui_coordinates_C): the REAL dish-aim state + its repaint verb.
// The widget is a SINGLETON child of the desk's atlas widget, reached through
// the desk's own field chain (desk.Widget -> atlas.ui_coordinates -- the
// game's access path, getCoordWidget). Split out of console_desk.cpp
// 2026-07-19 (one engine class per ue_wrap file); bodies verbatim.
// Principle-7 engine-wrapper layer -- NO network logic; coop::desk_cursor_sync
// (the 60Hz live cursor stream) + coop::console_state_sync (the committed
// locks) drive the mirror through here. Game thread.

#pragma once

#include <cstdint>

namespace ue_wrap::coords_panel {

// The coords-panel cursor state (the REAL dish aim -- the ui_coordinates
// widget fields; spaceRenderer.coords is dead bytecode). Receivers raw-write
// + the reflected updCursorLocations() repaint, which also rotates the
// physical pingDishes (setRot).
struct DishAim {
    float viewX = 0, viewY = 0;   // viewCoordinate
    float c0X = 0, c0Y = 0;       // Coordinate_0
    float c1X = 0, c1Y = 0;       // Coordinate_1
    float c2X = 0, c2Y = 0;       // Coordinate_2
    int32_t selected = 0;         // the selected cursor index
    uint8_t direction = 0;        // Direction @0x441 -- the catch-gate toggle (v70)
};

// The live validated ui_coordinates_C instance (cached + index-revalidated;
// resolved via console_desk::AtlasUiCoordsSlot). Null when the desk/atlas/
// widget chain is not live yet. Self-resolving (throttled 2s lazy retry) for
// the WIDGET half only -- the desk half of the chain (desk.Widget/atlas
// offsets) resolves via console_desk::EnsureResolved, which every current
// sync caller already gates on before reaching this API.
void* Instance();

bool ReadDishAim(DishAim& out);
// v109: the LIVE cursor and the COMMITTED locks are SEPARATE writes (see the
// .cpp). WriteCursorOnly = viewCoordinate memcpy, NO dispatch (the 60Hz interpolated
// stream; the widget's own Tick repaints). WriteDishCommitted = the discrete locks +
// updCursorLocations repaint (commit rate). WriteDishAim (wrote both + dispatched at
// 3Hz) is RETIRED -- two authors on viewCoordinate is the dupe shape (RULE 2).
bool WriteCursorOnly(float viewX, float viewY);
bool WriteDishCommitted(const DishAim& in);

}  // namespace ue_wrap::coords_panel
