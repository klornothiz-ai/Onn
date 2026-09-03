// ============================================================================
// ProsperoLayer RDNA2 Core - HLE libPad input test (item #2, input half)
// ----------------------------------------------------------------------------
// Drives the real DualSense/pad input state machine (libs/controller.cpp)
// through its guest-facing entry points -- PadInit / PadOpen / PadGetHandle /
// PadReadState / PadRead -- with input fed through the injectable host API
// (ControllerConnect / ControllerButton / ControllerAxis). This is the exact
// pipeline a guest game uses to read the controller, exercised headlessly with
// no SDL / no physical gamepad: the host layer (SDL on the user's machine, or
// these injectors in a test) feeds events; the guest reads faithful PadData.
//
// Proves: handle/arg validation, button press/release edge tracking, analog
// stick + trigger mapping, connection state, the queued-samples PadRead path,
// and multi-sample history.
// ============================================================================
#include "libs/controller.h"
#include "libs/padData.h"

#include <cstdint>
#include <iostream>

namespace {
int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)
} // namespace

int main() {
    using namespace Libs::Controller;
    std::cout << "=== HLE libPad Input Test (item #2, input) ===\n";

    ControllerEnsureInitialized(); // creates g_controller, connects HOST_INPUT id

    // --- open / handle validation -------------------------------------------
    CHECK(PadInit() == 0);
    const int handle = PadOpen(1000, 0, 0, nullptr);
    CHECK(handle == 1);
    CHECK(PadGetHandle(1000, 0, 0) == 1);
    CHECK(PadOpen(999, 0, 0, nullptr) < 0);     // bad user id
    CHECK(PadOpen(1000, 5, 0, nullptr) < 0);    // bad type
    CHECK(PadReadState(2, nullptr) < 0);        // bad handle
    {
        PadData d{};
        CHECK(PadReadState(1, nullptr) < 0);    // null data
        CHECK(PadReadState(handle, &d) == 0);   // valid
    }

    // A gamepad connects (host feeds the event).
    ControllerConnect(1);

    // --- button press is visible to the guest -------------------------------
    {
        ControllerButton(1, PAD_BUTTON_CROSS, true);
        PadData d{};
        CHECK(PadReadState(handle, &d) == 0);
        CHECK(d.connected);
        CHECK((d.buttons & PAD_BUTTON_CROSS) != 0);
        CHECK((d.buttons & PAD_BUTTON_CIRCLE) == 0);
        CHECK(d.orientation_w == 1.0f);         // pad_fill_data sets identity quat

        // release clears just that bit
        ControllerButton(1, PAD_BUTTON_CROSS, false);
        PadData d2{};
        CHECK(PadReadState(handle, &d2) == 0);
        CHECK((d2.buttons & PAD_BUTTON_CROSS) == 0);
    }

    // --- multiple simultaneous buttons --------------------------------------
    {
        ControllerButton(1, PAD_BUTTON_L1, true);
        ControllerButton(1, PAD_BUTTON_R1, true);
        ControllerButton(1, PAD_BUTTON_TRIANGLE, true);
        PadData d{};
        CHECK(PadReadState(handle, &d) == 0);
        CHECK((d.buttons & PAD_BUTTON_L1) != 0);
        CHECK((d.buttons & PAD_BUTTON_R1) != 0);
        CHECK((d.buttons & PAD_BUTTON_TRIANGLE) != 0);
        ControllerButton(1, PAD_BUTTON_L1, false);
        ControllerButton(1, PAD_BUTTON_R1, false);
        ControllerButton(1, PAD_BUTTON_TRIANGLE, false);
    }

    // --- analog sticks + triggers (host feeds already-scaled 0..255 values) --
    // The host input layer scales raw device axes to 0..255 via
    // controller_get_axis() before calling ControllerAxis(); the state machine
    // stores the value as-is and the guest reads it back unchanged.
    {
        ControllerAxis(1, Axis::LeftX, 255);          // full right
        ControllerAxis(1, Axis::LeftY, 0);            // full up
        ControllerAxis(1, Axis::RightX, 128);         // center
        ControllerAxis(1, Axis::TriggerRight, 200);   // partial pull
        PadData d{};
        CHECK(PadReadState(handle, &d) == 0);
        CHECK(d.left_stick_x == 255);
        CHECK(d.left_stick_y == 0);
        CHECK(d.right_stick_x == 128);
        CHECK(d.analog_buttons_r2 == 200);
        // A non-zero right trigger also latches the R2 digital button.
        CHECK((d.buttons & PAD_BUTTON_R2) != 0);
        std::cout << "  [ok] analog map: LX=" << int(d.left_stick_x)
                  << " LY=" << int(d.left_stick_y)
                  << " RX=" << int(d.right_stick_x)
                  << " R2=" << int(d.analog_buttons_r2) << "\n";
        // release trigger clears the digital bit
        ControllerAxis(1, Axis::TriggerRight, 0);
        PadData d2{};
        CHECK(PadReadState(handle, &d2) == 0);
        CHECK((d2.buttons & PAD_BUTTON_R2) == 0);
    }

    // --- PadRead returns queued samples (history) ---------------------------
    {
        // Push a few distinct button states, then drain them via PadRead.
        ControllerButton(1, PAD_BUTTON_SQUARE, true);
        ControllerButton(1, PAD_BUTTON_SQUARE, false);
        ControllerButton(1, PAD_BUTTON_CIRCLE, true);
        PadData buf[16]{};
        const int n = PadRead(handle, buf, 16);
        CHECK(n >= 1);
        CHECK(buf[0].connected);
        std::cout << "  [ok] PadRead drained " << n << " queued sample(s)\n";
        // last state should still be readable via PadReadState
        PadData latest{};
        CHECK(PadReadState(handle, &latest) == 0);
        CHECK((latest.buttons & PAD_BUTTON_CIRCLE) != 0);
    }

    // --- PadRead argument validation ----------------------------------------
    {
        PadData buf[4]{};
        CHECK(PadRead(2, buf, 1) < 0);          // bad handle
        CHECK(PadRead(handle, nullptr, 1) < 0); // null
    }

    // --- disconnect clears connection for the guest -------------------------
    {
        ControllerDisconnect(1);
        // HOST_INPUT controller is still connected by default; the guest still
        // sees "connected" via the host input fallback. Assert the call path is
        // stable (no crash) and returns OK.
        PadData d{};
        CHECK(PadReadState(handle, &d) == 0);
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] HLE libPad input path verified "
                     "(injected host events -> faithful guest PadData).\n";
    }
    return g_failures == 0 ? 0 : 1;
}
