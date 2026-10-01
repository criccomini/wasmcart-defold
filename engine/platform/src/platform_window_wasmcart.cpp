// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// wasmcart window backend.
//
// A cart does not own a window and does not own the frame loop: the host
// calls wc_render() once per frame and the cart returns. So this backend
// holds the window state the engine asks about, and reads input out of the
// shared-memory blocks the host writes before each call.
//
// Derived from platform_window_null.cpp, which already answers this
// interface; the difference is that input comes from the cart ABI rather
// than being stubbed to zero.

#include <string.h>
#include <dlib/log.h>

#include "window.hpp"
#include "platform_window_constants.h"
#include "platform_window_wasmcart.h"

struct dmWindow
{
    WindowCreateParams m_CreateParams;
    uint32_t           m_WindowWidth;
    uint32_t           m_WindowHeight;
    uint32_t           m_WindowOpened             : 1;
    uint32_t           m_StateCursor              : 1;
    uint32_t           m_StateCursorLock          : 1;
    uint32_t           m_StateCursorAccelerometer : 1;
    uint32_t           m_StateKeyboard            : 3;
};

namespace dmPlatform
{
    // Input blocks owned by the cart shim (platform_wasmcart_input.cpp).
    // Null until the shim registers them, so every getter tolerates absence.
    static const WasmcartInputState* g_Input = 0;

    void WasmcartSetInputState(const WasmcartInputState* state)
    {
        g_Input = state;
    }

    HWindow NewWindow()
    {
        dmWindow* wnd = new dmWindow();
        memset(wnd, 0, sizeof(dmWindow));
        return wnd;
    }

    void DeleteWindow(HWindow window)
    {
        delete window;
    }

    WindowResult OpenWindow(HWindow window, const WindowCreateParams& params)
    {
        if (window->m_WindowOpened)
        {
            return WINDOW_RESULT_WINDOW_ALREADY_OPENED;
        }

        window->m_CreateParams = params;
        window->m_WindowOpened = 1;
        window->m_WindowWidth  = params.m_Width;
        window->m_WindowHeight = params.m_Height;

        return WINDOW_RESULT_OK;
    }

    void CloseWindow(HWindow window)
    {
        window->m_WindowOpened = 0;
        window->m_WindowWidth  = 0;
        window->m_WindowHeight = 0;
    }

    uint32_t GetWindowWidth(HWindow window)
    {
        return window->m_WindowWidth;
    }

    uint32_t GetWindowHeight(HWindow window)
    {
        return window->m_WindowHeight;
    }

    bool GetSafeArea(HWindow window, WindowSafeArea* out)
    {
        const uint32_t width = GetWindowWidth(window);
        const uint32_t height = GetWindowHeight(window);

        out->m_X = 0;
        out->m_Y = 0;
        out->m_Width = width;
        out->m_Height = height;
        out->m_InsetLeft = 0;
        out->m_InsetTop = 0;
        out->m_InsetRight = 0;
        out->m_InsetBottom = 0;

        return true;
    }

    uint32_t GetWindowStateParam(HWindow window, WindowState state)
    {
        switch(state)
        {
            case WINDOW_STATE_OPENED: return window->m_WindowOpened;
            case WINDOW_STATE_FSAA_SAMPLES: return window->m_CreateParams.m_Samples;
            default:break;
        }

        return 0;
    }

    float GetDisplayScaleFactor(HWindow window)
    {
        return 1.0f;
    }

    uintptr_t GetProcAddress(HWindow window, const char* proc_name)
    {
        // GL entry points are wasm imports resolved at link time, not looked
        // up at runtime. Returning 0 makes the engine take its static path.
        return 0;
    }

    // Defold's key constants must be a dense range: dmHID subtracts
    // PLATFORM_KEY_START and indexes a fixed MAX_KEY_COUNT bitfield with the
    // result (hid.cpp GetKey/SetKey). The wasmcart host, by contrast, reports
    // a 256-bit mask indexed by USB HID scancode. So the constants below stay
    // dense, matching the null/GLFW backends, and this table maps each one to
    // its HID scancode. 0 means "no HID equivalent" and always reads as up.
    static const uint8_t WASMCART_KEY_TO_HID[] = {
        0x00, // PLATFORM_KEY_START (unused sentinel)
        0x29, // ESC
        0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, // F1..F6
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, // F7..F12
        0x52, // UP
        0x51, // DOWN
        0x50, // LEFT
        0x4F, // RIGHT
        0xE1, // LSHIFT
        0xE5, // RSHIFT
        0xE0, // LCTRL
        0xE4, // RCTRL
        0xE2, // LALT
        0xE6, // RALT
        0x2B, // TAB
        0x28, // ENTER
        0x2A, // BACKSPACE
        0x49, // INSERT
        0x4C, // DEL
        0x4B, // PAGEUP
        0x4E, // PAGEDOWN
        0x4A, // HOME
        0x4D, // END
        0x62, // KP_0
        0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, // KP_1..KP_9
        0x54, // KP_DIVIDE
        0x55, // KP_MULTIPLY
        0x56, // KP_SUBTRACT
        0x57, // KP_ADD
        0x63, // KP_DECIMAL
        0x67, // KP_EQUAL
        0x58, // KP_ENTER
        0x53, // KP_NUM_LOCK
        0x39, // CAPS_LOCK
        0x47, // SCROLL_LOCK
        0x48, // PAUSE
        0xE3, // LSUPER
        0xE7, // RSUPER
        0x65, // MENU
        0x00, // BACK (Android only; no HID equivalent)
    };

    int32_t GetKey(HWindow window, int32_t code)
    {
        if (!g_Input || !g_Input->m_Keys || code < 0)
        {
            return 0;
        }

        uint8_t hid;
        if (code < (int32_t) (sizeof(WASMCART_KEY_TO_HID) / sizeof(WASMCART_KEY_TO_HID[0])))
        {
            hid = WASMCART_KEY_TO_HID[code];
        }
        else
        {
            // Printable ASCII arrives as its character code, the way GLFW
            // reports it; map that onto the HID letter/digit block.
            if (code >= 'A' && code <= 'Z')      hid = 0x04 + (code - 'A');
            else if (code >= 'a' && code <= 'z') hid = 0x04 + (code - 'a');
            else if (code >= '1' && code <= '9') hid = 0x1E + (code - '1');
            else if (code == '0')                hid = 0x27;
            else if (code == ' ')                hid = 0x2C;
            else if (code == '-')                hid = 0x2D;
            else if (code == '=')                hid = 0x2E;
            else if (code == '[')                hid = 0x2F;
            else if (code == ']')                hid = 0x30;
            else if (code == '\\')               hid = 0x31;
            else if (code == ';')                hid = 0x33;
            else if (code == '\'')               hid = 0x34;
            else if (code == '`')                hid = 0x35;
            else if (code == ',')                hid = 0x36;
            else if (code == '.')                hid = 0x37;
            else if (code == '/')                hid = 0x38;
            else                                 hid = 0x00;
        }

        if (hid == 0)
        {
            return 0;
        }
        return (g_Input->m_Keys[hid >> 3] & (1 << (hid & 7))) ? 1 : 0;
    }

    int32_t GetMouseButton(HWindow window, int32_t button)
    {
        if (!g_Input || !g_Input->m_Pointers || button < 0 || button > 2)
        {
            return 0;
        }
        // Pointer 0 is the mouse; touches occupy the remaining slots.
        const WasmcartPointer* p = &g_Input->m_Pointers[0];
        if (!p->m_Active)
        {
            return 0;
        }
        return (p->m_Buttons & (1 << button)) ? 1 : 0;
    }

    int32_t GetMouseWheel(HWindow window)
    {
        if (!g_Input || !g_Input->m_Wheel)
        {
            return 0;
        }
        // The cart ABI reports 1/120 of a notch; the engine wants notches.
        return g_Input->m_Wheel->m_DY / 120;
    }

    void GetMousePosition(HWindow window, int32_t* x, int32_t* y)
    {
        int32_t px = 0;
        int32_t py = 0;
        if (g_Input && g_Input->m_Pointers && g_Input->m_Pointers[0].m_Active)
        {
            px = g_Input->m_Pointers[0].m_X;
            py = g_Input->m_Pointers[0].m_Y;
        }
        if (x) *x = px;
        if (y) *y = py;
    }

    // Touch phases, matching dmHID::Phase. The platform layer deliberately
    // does not include hid.h (hid depends on platform, not the other way
    // round), so the values are mirrored here the way the GLFW backend also
    // passes them through as plain ints.
    enum WasmcartTouchPhase
    {
        WASMCART_PHASE_BEGAN      = 0,
        WASMCART_PHASE_MOVED      = 1,
        WASMCART_PHASE_STATIONARY = 2,
        WASMCART_PHASE_ENDED      = 3,
    };

    // Previous-frame pointer state, so a press becomes BEGAN exactly once and
    // a release becomes ENDED exactly once. Reporting MOVED every frame would
    // make dmHID see a touch that never begins and never ends.
    static int32_t  g_PrevX[WASMCART_POINTER_COUNT];
    static int32_t  g_PrevY[WASMCART_POINTER_COUNT];
    static uint8_t  g_PrevDown[WASMCART_POINTER_COUNT];

    uint32_t GetTouchData(HWindow window, WindowTouchData* touch_data, uint32_t touch_data_count)
    {
        if (!g_Input || !g_Input->m_Pointers || !touch_data || touch_data_count == 0)
        {
            return 0;
        }

        uint32_t count = 0;
        for (uint32_t i = 0; i < WASMCART_POINTER_COUNT && count < touch_data_count; ++i)
        {
            const WasmcartPointer* p = &g_Input->m_Pointers[i];
            const uint8_t down = (p->m_Active && (p->m_Buttons & 1)) ? 1 : 0;
            const uint8_t was_down = g_PrevDown[i];

            // An inactive slot that was not down last frame has nothing to say.
            if (!down && !was_down)
            {
                g_PrevX[i] = p->m_X;
                g_PrevY[i] = p->m_Y;
                continue;
            }

            int32_t phase;
            if (down && !was_down)
            {
                phase = WASMCART_PHASE_BEGAN;
            }
            else if (!down && was_down)
            {
                phase = WASMCART_PHASE_ENDED;
            }
            else if (p->m_X != g_PrevX[i] || p->m_Y != g_PrevY[i])
            {
                phase = WASMCART_PHASE_MOVED;
            }
            else
            {
                phase = WASMCART_PHASE_STATIONARY;
            }

            WindowTouchData* td = &touch_data[count];
            memset(td, 0, sizeof(WindowTouchData));
            td->m_Id       = (int32_t) i;
            td->m_X        = p->m_X;
            td->m_Y        = p->m_Y;
            td->m_DX       = p->m_X - g_PrevX[i];
            td->m_DY       = p->m_Y - g_PrevY[i];
            td->m_TapCount = down ? 1 : 0;
            td->m_Phase    = phase;
            count++;

            g_PrevDown[i] = down;
            g_PrevX[i]    = p->m_X;
            g_PrevY[i]    = p->m_Y;
        }
        return count;
    }

    bool GetAcceleration(HWindow window, float* x, float* y, float* z)
    {
        // No accelerometer in the cart ABI.
        if (x) *x = 0.0f;
        if (y) *y = 0.0f;
        if (z) *z = 0.0f;
        return false;
    }

    const char* GetJoystickDeviceName(HWindow window, uint32_t joystick_index)
    {
        if (!g_Input || joystick_index >= WASMCART_PAD_COUNT)
        {
            return 0;
        }
        return g_Input->m_PadNames[joystick_index];
    }

    const char* GetJoystickDeviceGuid(HWindow window, uint32_t joystick_index)
    {
        return 0;
    }

    uint32_t GetJoystickAxes(HWindow window, uint32_t joystick_index, float* values, uint32_t values_capacity)
    {
        if (!g_Input || !g_Input->m_Pads || joystick_index >= WASMCART_PAD_COUNT || !values)
        {
            return 0;
        }
        const WasmcartPad* pad = &g_Input->m_Pads[joystick_index];
        if (!pad->m_Connected)
        {
            return 0;
        }

        // Sticks are int16; triggers are uint8. Both normalise to -1..1 / 0..1.
        const float axes[6] = {
            pad->m_LeftX  / 32767.0f,
            pad->m_LeftY  / 32767.0f,
            pad->m_RightX / 32767.0f,
            pad->m_RightY / 32767.0f,
            pad->m_LeftTrigger  / 255.0f,
            pad->m_RightTrigger / 255.0f,
        };

        uint32_t count = values_capacity < 6 ? values_capacity : 6;
        for (uint32_t i = 0; i < count; ++i)
        {
            values[i] = axes[i];
        }
        return count;
    }

    uint32_t GetJoystickHats(HWindow window, uint32_t joystick_index, uint8_t* values, uint32_t values_capacity)
    {
        // The d-pad arrives as buttons, not as a hat.
        return 0;
    }

    uint32_t GetJoystickButtons(HWindow window, uint32_t joystick_index, uint8_t* values, uint32_t values_capacity)
    {
        if (!g_Input || !g_Input->m_Pads || joystick_index >= WASMCART_PAD_COUNT || !values)
        {
            return 0;
        }
        const WasmcartPad* pad = &g_Input->m_Pads[joystick_index];
        if (!pad->m_Connected)
        {
            return 0;
        }

        uint32_t count = values_capacity < WASMCART_PAD_BUTTON_COUNT
                       ? values_capacity : WASMCART_PAD_BUTTON_COUNT;
        for (uint32_t i = 0; i < count; ++i)
        {
            values[i] = (pad->m_Buttons & (1 << i)) ? 1 : 0;
        }
        return count;
    }

    void SetWindowTitle(HWindow window, const char* title)
    {
        // A cart has no title bar.
    }

    void SetWindowSize(HWindow window, uint32_t width, uint32_t height)
    {
        window->m_WindowWidth  = width;
        window->m_WindowHeight = height;

        if (window->m_CreateParams.m_ResizeCallback)
        {
            window->m_CreateParams.m_ResizeCallback(window->m_CreateParams.m_ResizeCallbackUserData, width, height);
        }
    }

    void SetWindowPosition(HWindow window, int32_t x, int32_t y)
    {
        // The host owns placement.
    }

    void ShowWindow(HWindow window)
    {}

    void HideWindow(HWindow window)
    {}

    void SetSwapInterval(HWindow window, uint32_t swap_interval)
    {
        // The host paces frames; the cart does not choose a swap interval.
    }

    void IconifyWindow(HWindow window)
    {}

    void PollEvents(HWindow window)
    {
        // Input is shared memory the host refreshes before each wc_render();
        // there is no event queue to drain.
    }

    void SwapBuffers(HWindow window)
    {
        // Presentation happens when wc_render() returns.
    }

    void SetDeviceState(HWindow window, WindowDeviceState state, bool op1)
    {
        SetDeviceState(window, state, op1, false);
    }

    void SetDeviceState(HWindow window, WindowDeviceState state, bool op1, bool op2)
    {
        switch(state)
        {
            case WINDOW_DEVICE_STATE_CURSOR:
                window->m_StateCursor = op1;
                window->m_StateCursorLock = !op1;
                break;
            case WINDOW_DEVICE_STATE_CURSOR_LOCK:
                break;
            case WINDOW_DEVICE_STATE_ACCELEROMETER:
                window->m_StateCursorAccelerometer = op1;
                break;
            case WINDOW_DEVICE_STATE_KEYBOARD_DEFAULT:
                window->m_StateKeyboard = op1 ? 1 : 0;
                WasmcartSetTextInput(op1);
                break;
            case WINDOW_DEVICE_STATE_KEYBOARD_NUMBER_PAD:
                window->m_StateKeyboard = op1 ? 2 : 0;
                WasmcartSetTextInput(op1);
                break;
            case WINDOW_DEVICE_STATE_KEYBOARD_EMAIL:
                window->m_StateKeyboard = op1 ? 3 : 0;
                WasmcartSetTextInput(op1);
                break;
            case WINDOW_DEVICE_STATE_KEYBOARD_PASSWORD:
                window->m_StateKeyboard = op1 ? 4 : 0;
                WasmcartSetTextInput(op1);
                break;
            default:
                dmLogWarning("Unable to set device state (%d), unknown state.", (int) state);
                break;
        }
    }

    bool GetDeviceState(HWindow window, WindowDeviceState state)
    {
        switch(state)
        {
            case WINDOW_DEVICE_STATE_CURSOR:              return window->m_StateCursor;
            case WINDOW_DEVICE_STATE_CURSOR_LOCK:         return window->m_StateCursorLock;
            case WINDOW_DEVICE_STATE_ACCELEROMETER:       return window->m_StateCursorAccelerometer;
            case WINDOW_DEVICE_STATE_KEYBOARD_DEFAULT:    return window->m_StateKeyboard == 1;
            case WINDOW_DEVICE_STATE_KEYBOARD_NUMBER_PAD: return window->m_StateKeyboard == 2;
            case WINDOW_DEVICE_STATE_KEYBOARD_EMAIL:      return window->m_StateKeyboard == 3;
            case WINDOW_DEVICE_STATE_KEYBOARD_PASSWORD:   return window->m_StateKeyboard == 4;
            default:break;
        }
        return false;
    }

    bool GetDeviceState(HWindow window, WindowDeviceState state, int32_t op1)
    {
        return GetDeviceState(window, state);
    }

    void SetKeyboardCharCallback(HWindow window, FWindowAddKeyboardCharCallback cb, void* user_data)
    {
        // Text input arrives through the host's text-input surface rather
        // than as per-character window events.
    }

    void SetKeyboardMarkedTextCallback(HWindow window, FWindowSetMarkedTextCallback cb, void* user_data)
    {
    }

    void SetKeyboardDeviceChangedCallback(HWindow window, FWindowDeviceChangedCallback cb, void* user_data)
    {
    }

    void SetGamepadEventCallback(HWindow window, FWindowGamepadEventCallback cb, void* user_data)
    {
        // Pads are polled out of shared memory; the host sends no connect or
        // disconnect events.
    }

    bool WasmcartGetPad(uint32_t index, WasmcartPad* out)
    {
        if (!g_Input || !g_Input->m_Pads || !out || index >= WASMCART_PAD_COUNT)
        {
            return false;
        }
        *out = g_Input->m_Pads[index];
        return true;
    }

    const char* WasmcartGetPadName(uint32_t index)
    {
        if (!g_Input || index >= WASMCART_PAD_COUNT)
        {
            return 0;
        }
        return g_Input->m_PadNames[index];
    }

    void* AcquireAuxContext(HWindow window)
    {
        // No second GL context: a cart gets exactly one from the host.
        return 0;
    }

    void UnacquireAuxContext(HWindow window, void* aux_context)
    {
    }

    int32_t TriggerCloseCallback(HWindow window)
    {
        if (window->m_CreateParams.m_CloseCallback)
        {
            return window->m_CreateParams.m_CloseCallback(window->m_CreateParams.m_CloseCallbackUserData);
        }
        return 0;
    }

    int32_t OpenGLGetDefaultFramebufferId()
    {
        // The host binds its own framebuffer before calling wc_render, and
        // WebGL's default framebuffer is 0.
        return 0;
    }

    // Dense key constants, matching platform_window_null.cpp. dmHID indexes a
    // fixed-size bitfield with (key - PLATFORM_KEY_START), so these cannot be
    // sparse HID scancodes; WASMCART_KEY_TO_HID above does that translation.
    const int PLATFORM_KEY_START           = 0;
    const int PLATFORM_JOYSTICK_LAST       = 0;
    const int PLATFORM_KEY_ESC             = 1;
    const int PLATFORM_KEY_F1              = 2;
    const int PLATFORM_KEY_F2              = 3;
    const int PLATFORM_KEY_F3              = 4;
    const int PLATFORM_KEY_F4              = 5;
    const int PLATFORM_KEY_F5              = 6;
    const int PLATFORM_KEY_F6              = 7;
    const int PLATFORM_KEY_F7              = 8;
    const int PLATFORM_KEY_F8              = 9;
    const int PLATFORM_KEY_F9              = 10;
    const int PLATFORM_KEY_F10             = 11;
    const int PLATFORM_KEY_F11             = 12;
    const int PLATFORM_KEY_F12             = 13;
    const int PLATFORM_KEY_UP              = 14;
    const int PLATFORM_KEY_DOWN            = 15;
    const int PLATFORM_KEY_LEFT            = 16;
    const int PLATFORM_KEY_RIGHT           = 17;
    const int PLATFORM_KEY_LSHIFT          = 18;
    const int PLATFORM_KEY_RSHIFT          = 19;
    const int PLATFORM_KEY_LCTRL           = 20;
    const int PLATFORM_KEY_RCTRL           = 21;
    const int PLATFORM_KEY_LALT            = 22;
    const int PLATFORM_KEY_RALT            = 23;
    const int PLATFORM_KEY_TAB             = 24;
    const int PLATFORM_KEY_ENTER           = 25;
    const int PLATFORM_KEY_BACKSPACE       = 26;
    const int PLATFORM_KEY_INSERT          = 27;
    const int PLATFORM_KEY_DEL             = 28;
    const int PLATFORM_KEY_PAGEUP          = 29;
    const int PLATFORM_KEY_PAGEDOWN        = 30;
    const int PLATFORM_KEY_HOME            = 31;
    const int PLATFORM_KEY_END             = 32;
    const int PLATFORM_KEY_KP_0            = 33;
    const int PLATFORM_KEY_KP_1            = 34;
    const int PLATFORM_KEY_KP_2            = 35;
    const int PLATFORM_KEY_KP_3            = 36;
    const int PLATFORM_KEY_KP_4            = 37;
    const int PLATFORM_KEY_KP_5            = 38;
    const int PLATFORM_KEY_KP_6            = 39;
    const int PLATFORM_KEY_KP_7            = 40;
    const int PLATFORM_KEY_KP_8            = 41;
    const int PLATFORM_KEY_KP_9            = 42;
    const int PLATFORM_KEY_KP_DIVIDE       = 43;
    const int PLATFORM_KEY_KP_MULTIPLY     = 44;
    const int PLATFORM_KEY_KP_SUBTRACT     = 45;
    const int PLATFORM_KEY_KP_ADD          = 46;
    const int PLATFORM_KEY_KP_DECIMAL      = 47;
    const int PLATFORM_KEY_KP_EQUAL        = 48;
    const int PLATFORM_KEY_KP_ENTER        = 49;
    const int PLATFORM_KEY_KP_NUM_LOCK     = 50;
    const int PLATFORM_KEY_CAPS_LOCK       = 51;
    const int PLATFORM_KEY_SCROLL_LOCK     = 52;
    const int PLATFORM_KEY_PAUSE           = 53;
    const int PLATFORM_KEY_LSUPER          = 54;
    const int PLATFORM_KEY_RSUPER          = 55;
    const int PLATFORM_KEY_MENU            = 56;
    const int PLATFORM_KEY_BACK            = 57;

    const int PLATFORM_MOUSE_BUTTON_LEFT   = 0;
    const int PLATFORM_MOUSE_BUTTON_RIGHT  = 1;
    const int PLATFORM_MOUSE_BUTTON_MIDDLE = 2;
    const int PLATFORM_MOUSE_BUTTON_1      = 0;
    const int PLATFORM_MOUSE_BUTTON_2      = 1;
    const int PLATFORM_MOUSE_BUTTON_3      = 2;
    const int PLATFORM_MOUSE_BUTTON_4      = 3;
    const int PLATFORM_MOUSE_BUTTON_5      = 4;
    const int PLATFORM_MOUSE_BUTTON_6      = 5;
    const int PLATFORM_MOUSE_BUTTON_7      = 6;
    const int PLATFORM_MOUSE_BUTTON_8      = 7;

    const int PLATFORM_JOYSTICK_1          = 0;
    const int PLATFORM_JOYSTICK_2          = 1;
    const int PLATFORM_JOYSTICK_3          = 2;
    const int PLATFORM_JOYSTICK_4          = 3;
    const int PLATFORM_JOYSTICK_5          = 4;
    const int PLATFORM_JOYSTICK_6          = 5;
    const int PLATFORM_JOYSTICK_7          = 6;
    const int PLATFORM_JOYSTICK_8          = 7;
    const int PLATFORM_JOYSTICK_9          = 8;
    const int PLATFORM_JOYSTICK_10         = 9;
    const int PLATFORM_JOYSTICK_11         = 10;
    const int PLATFORM_JOYSTICK_12         = 11;
    const int PLATFORM_JOYSTICK_13         = 12;
    const int PLATFORM_JOYSTICK_14         = 13;
    const int PLATFORM_JOYSTICK_15         = 14;
    const int PLATFORM_JOYSTICK_16         = 15;
}
