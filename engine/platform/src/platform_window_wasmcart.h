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

#ifndef DM_PLATFORM_WINDOW_WASMCART_H
#define DM_PLATFORM_WINDOW_WASMCART_H

#include <stdint.h>

// Mirrors of the wasmcart ABI v4 input blocks.
//
// These are layout-compatible with wc_pad_t / wc_pointer_t / wc_wheel_t in
// wasmcart.h, but declared separately so the engine's platform layer does
// not have to include the cart header (which pulls in wasm import
// attributes that only make sense in the cart shim translation unit).
// The sizes are asserted below rather than against the cart header: that
// cross-check was described in a comment here and never actually existed, so
// nothing stopped these mirrors from drifting when the ABI moved to v4 -- and
// they did. A size assert cannot catch a reordering, but it catches every
// change that alters the footprint, which is what v4 was.

// Button bits in WasmcartPad::m_Buttons, mirroring WC_BTN_* in wasmcart.h.
// Named separately for the same reason the structs are: the platform layer
// must not pull in the cart header's wasm import attributes.
#define WASMCART_BTN_A      (1 << 0)
#define WASMCART_BTN_B      (1 << 1)
#define WASMCART_BTN_X      (1 << 2)
#define WASMCART_BTN_Y      (1 << 3)
#define WASMCART_BTN_L      (1 << 4)
#define WASMCART_BTN_R      (1 << 5)
#define WASMCART_BTN_START  (1 << 6)
#define WASMCART_BTN_SELECT (1 << 7)
#define WASMCART_BTN_UP     (1 << 8)
#define WASMCART_BTN_DOWN   (1 << 9)
#define WASMCART_BTN_LEFT   (1 << 10)
#define WASMCART_BTN_RIGHT  (1 << 11)
#define WASMCART_BTN_L3     (1 << 12)
#define WASMCART_BTN_R3     (1 << 13)
// ABI v4 additions, completing parity with SDL2's controller button set.
#define WASMCART_BTN_GUIDE    (1 << 14)
#define WASMCART_BTN_MISC1    (1 << 15)
#define WASMCART_BTN_PADDLE1  (1 << 16)
#define WASMCART_BTN_PADDLE2  (1 << 17)
#define WASMCART_BTN_PADDLE3  (1 << 18)
#define WASMCART_BTN_PADDLE4  (1 << 19)
#define WASMCART_BTN_TOUCHPAD (1 << 20)

// Full travel on a trigger (ABI v4). Matches SDL2 and libretro.
#define WASMCART_TRIGGER_MAX  32767

#define WASMCART_POINTER_COUNT    10
#define WASMCART_PAD_COUNT        4
#define WASMCART_PAD_BUTTON_COUNT 21
#define WASMCART_KEY_COUNT        256

namespace dmPlatform
{
    // 20 bytes (ABI v4). Every analog axis is int16: sticks -32768..32767,
    // triggers 0..32767, which is what SDL2 and libretro report natively.
    struct WasmcartPad
    {
        uint32_t m_Buttons;        // bits 21-31 reserved
        int16_t  m_LeftX;
        int16_t  m_LeftY;
        int16_t  m_RightX;
        int16_t  m_RightY;
        int16_t  m_LeftTrigger;    // 0..32767, never negative
        int16_t  m_RightTrigger;   // 0..32767, never negative
        uint8_t  m_Connected;
        uint8_t  m_Pad[3];
    };
    static_assert(sizeof(WasmcartPad) == 20, "WasmcartPad must match wc_pad_t (ABI v4)");

    struct WasmcartPointer
    {
        int16_t m_X;
        int16_t m_Y;
        uint8_t m_Buttons;
        uint8_t m_Active;
        uint8_t m_Pad[2];
    };

    struct WasmcartWheel
    {
        int32_t m_DX;
        int32_t m_DY;
    };

    // Pointers into the shared blocks the host refreshes before each frame.
    // Any member may be null when the cart did not request that input family.
    struct WasmcartInputState
    {
        const WasmcartPad*     m_Pads;
        const WasmcartPointer* m_Pointers;
        const WasmcartWheel*   m_Wheel;
        const uint8_t*         m_Keys;
        const char*            m_PadNames[WASMCART_PAD_COUNT];
    };

    // Registered once by the cart shim, before the engine is created.
    void WasmcartSetInputState(const WasmcartInputState* state);

    // Reads one host pad block. Returns false when the cart did not request
    // pad input or the index is out of range, which reads as "not connected"
    // rather than as an error. The HID gamepad driver polls through here for
    // the same reason GetKey exists: the host refreshes shared memory before
    // each frame and sends no events.
    bool WasmcartGetPad(uint32_t index, WasmcartPad* out);

    // The host's name for a pad, or null when it reported none.
    const char* WasmcartGetPadName(uint32_t index);

    // Persistent save storage, backed by the ABI's save block (the host
    // persists that region across sessions). A cart has no filesystem, so
    // sys.save/sys.load route here instead of through fopen.
    //
    // Capacities are fixed because the block is a flat byte region the host
    // copies verbatim: nothing inside it may be a pointer, so there is no
    // allocator and no growth. 8 files x 16 KB covers settings, progress and
    // high scores, which is what sys.save exists for.
#define WASMCART_SAVE_MAX_FILES 8
#define WASMCART_SAVE_MAX_NAME  128
#define WASMCART_SAVE_MAX_FILE  (16 * 1024)

    void*    WasmcartGetSaveBlock();
    uint32_t WasmcartGetSaveBlockSize();

    bool WasmcartSaveWrite(const char* name, const void* data, uint32_t size);
    // Returns false when the name is not present. out_size always receives the
    // stored size, which may exceed `capacity`; the copy is truncated, never
    // the reported size, so a caller can tell a short buffer from a short file.
    bool WasmcartSaveRead(const char* name, void* data, uint32_t capacity, uint32_t* out_size);
    bool WasmcartSaveExists(const char* name);
    bool WasmcartSaveUnlink(const char* name);

    // Raises/lowers the host's text-input affordance (an on-screen keyboard
    // where the host has one). Implemented by the cart shim.
    void WasmcartSetTextInput(bool enabled);
}

#endif // DM_PLATFORM_WINDOW_WASMCART_H
