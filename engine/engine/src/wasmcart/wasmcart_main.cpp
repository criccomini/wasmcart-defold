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

// wasmcart cart entry points.
//
// This is the loop inversion. A normal Defold binary calls
// dmEngine::RunLoop(), which owns the frame loop and does not return. A cart
// does not own the loop: the host calls wc_render() once per frame and the
// cart returns. So this file drives the same engine C API that
// engine_main.cpp drives -- dmEngineCreate once, dmEngineUpdate per frame --
// without RunLoop in the picture at all.

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "engine.h"
#include "engine_private.h"
#include <platform_window_wasmcart.h>
#include <dlib/time.h>

extern "C" void dmExportedSymbols(); // Found in "__exported_symbols.cpp"

// ---------------------------------------------------------------------------
// wasmcart ABI v3 (mirrors wasmcart.h; see platform_window_wasmcart.h for why
// the cart header itself is not included in the engine's platform layer).
// ---------------------------------------------------------------------------

#define WC_ABI_VERSION     3
#define WC_GPU_API_GLES3   1
#define WC_FLAG_AUDIO_F32  (1 << 0)
#define WC_FLAG_POINTER    (1 << 3)
#define WC_FLAG_KEYBOARD   (1 << 4)

// The sound device (device_wasmcart.cpp) owns the ring; the shim only
// publishes its address so the host knows where to drain from.
namespace dmDeviceWasmcart
{
    extern float    g_AudioRing[];
    extern uint32_t g_AudioWriteCursor;
    uint32_t GetRingFrameCount();
    void     SetMixRate(uint32_t rate);
}

struct wc_info_t
{
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fb_ptr;
    uint32_t audio_ptr;
    uint32_t audio_cap;
    uint32_t audio_write_ptr;
    uint32_t input_ptr;
    uint32_t save_ptr;
    uint32_t save_size;
    uint32_t time_ptr;
    uint32_t host_info_ptr;
    uint32_t flags;
    uint32_t audio_sample_rate;
    uint32_t pointer_ptr;
    uint32_t keys_ptr;
    uint32_t gpu_api;
    uint32_t wheel_ptr;
};

struct wc_time_t
{
    double   time_ms;
    double   delta_ms;
    uint32_t frame;
};

struct wc_host_info_t
{
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t _reserved0;
    uint32_t audio_sample_rate;
    uint32_t flags;
};

// The host writes these before each wc_render(); the cart only reads.
static wc_info_t                 g_Info;
static wc_time_t                 g_Time;
static wc_host_info_t            g_HostInfo;
static dmPlatform::WasmcartPad     g_Pads[WASMCART_PAD_COUNT];
static dmPlatform::WasmcartPointer g_Pointers[WASMCART_POINTER_COUNT];
static dmPlatform::WasmcartWheel   g_Wheel;
static uint8_t                     g_Keys[32];

static dmPlatform::WasmcartInputState g_InputState;

// Default cart resolution, overridden by the host's preference in wc_init().
// One host frame of virtual time. 60 Hz is the wasmcart frame contract; a
// host that steps slower still gets a consistent timeline, just slower wall
// time, which is the correct behaviour for a deterministic cart.
#define WASMCART_FRAME_US (1000000 / 60)

#define WASMCART_DEFAULT_WIDTH  960
#define WASMCART_DEFAULT_HEIGHT 540

static dmEngine::HEngine g_Engine      = 0;
static bool              g_EngineAlive = false;

#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_log")))
extern "C" void wc_log(const char* ptr, unsigned int len);
__attribute__((import_module("env"), import_name("wc_text_input_begin")))
extern "C" void wc_text_input_begin(void);
__attribute__((import_module("env"), import_name("wc_text_input_end")))
extern "C" void wc_text_input_end(void);
#else
extern "C" void wc_log(const char* ptr, unsigned int len) { (void)ptr; (void)len; }
extern "C" void wc_text_input_begin(void) {}
extern "C" void wc_text_input_end(void) {}
#endif

#define WC_LOG(s) wc_log(s, sizeof(s) - 1)

namespace dmPlatform
{
    // Declared in platform_window_wasmcart.h; the window backend calls this
    // when the engine raises or lowers a text field.
    void WasmcartSetTextInput(bool enabled)
    {
        if (enabled)
        {
            wc_text_input_begin();
        }
        else
        {
            wc_text_input_end();
        }
    }
}

// ---------------------------------------------------------------------------
// Cart exports
// ---------------------------------------------------------------------------

extern "C"
{

// Set once wc_init() has resolved the final resolution; read by wc_get_info()
// so a repeat call does not undo it.
static bool s_ResolutionResolved = false;
static void wasmcart_mark_resolution_resolved(void) { s_ResolutionResolved = true; }

__attribute__((export_name("wc_get_info")))
wc_info_t* wc_get_info(void)
{
    // wc_get_info() must be IDEMPOTENT: a host may call it again after
    // wc_init() to pick up whatever the cart resolved, and some do. Resetting
    // the resolution here would throw away the host's own preferred size that
    // wc_init() just adopted, handing that second call the 960x540 default
    // instead. That is exactly what happened on wasmcart-native, which
    // re-reads after init: a 1280x720 cart opened in a 960x540 window with its
    // picture cropped, while the JS host (which does not re-read) was fine.
    //
    // So remember whether the resolution has already been resolved, and keep
    // it across calls.
    uint32_t    resolved_w = g_Info.width;
    uint32_t    resolved_h = g_Info.height;

    memset(&g_Info, 0, sizeof(g_Info));
    g_Info.version     = WC_ABI_VERSION;
    g_Info.width       = s_ResolutionResolved ? resolved_w : WASMCART_DEFAULT_WIDTH;
    g_Info.height      = s_ResolutionResolved ? resolved_h : WASMCART_DEFAULT_HEIGHT;
    g_Info.gpu_api     = WC_GPU_API_GLES3;
    g_Info.flags       = WC_FLAG_AUDIO_F32 | WC_FLAG_POINTER | WC_FLAG_KEYBOARD;
    g_Info.audio_ptr       = (uint32_t)(uintptr_t) dmDeviceWasmcart::g_AudioRing;
    g_Info.audio_cap       = dmDeviceWasmcart::GetRingFrameCount();
    g_Info.audio_write_ptr = (uint32_t)(uintptr_t) &dmDeviceWasmcart::g_AudioWriteCursor;
    g_Info.input_ptr   = (uint32_t)(uintptr_t) g_Pads;
    g_Info.pointer_ptr = (uint32_t)(uintptr_t) g_Pointers;
    g_Info.keys_ptr    = (uint32_t)(uintptr_t) g_Keys;
    g_Info.wheel_ptr   = (uint32_t)(uintptr_t) &g_Wheel;
    g_Info.time_ptr    = (uint32_t)(uintptr_t) &g_Time;
    g_Info.host_info_ptr = (uint32_t)(uintptr_t) &g_HostInfo;
    // The save block is the cart's only persistent storage: the host copies
    // this region out at the end of a session and back in before the next
    // one, and sys.save/sys.load read and write inside it.
    g_Info.save_ptr    = (uint32_t)(uintptr_t) dmPlatform::WasmcartGetSaveBlock();
    g_Info.save_size   = dmPlatform::WasmcartGetSaveBlockSize();
    return &g_Info;
}

__attribute__((export_name("wc_init")))
void wc_init(void)
{
    // Point the window backend at the host's input blocks before the engine
    // exists, so the first frame already sees real input.
    memset(&g_InputState, 0, sizeof(g_InputState));
    g_InputState.m_Pads     = g_Pads;
    g_InputState.m_Pointers = g_Pointers;
    g_InputState.m_Wheel    = &g_Wheel;
    g_InputState.m_Keys     = g_Keys;
    dmPlatform::WasmcartSetInputState(&g_InputState);

    // The host states its mix rate; the device reports it back to the mixer.
    dmDeviceWasmcart::SetMixRate(g_HostInfo.audio_sample_rate
        ? g_HostInfo.audio_sample_rate : 48000);
    g_Info.audio_sample_rate = g_HostInfo.audio_sample_rate
        ? g_HostInfo.audio_sample_rate : 48000;

    // Honour the host's preferred resolution when it states one. Marked
    // resolved either way, so a later wc_get_info() keeps this answer rather
    // than resetting to the compiled-in default (see the note there).
    if (g_HostInfo.preferred_width && g_HostInfo.preferred_height)
    {
        g_Info.width  = g_HostInfo.preferred_width;
        g_Info.height = g_HostInfo.preferred_height;
    }
    wasmcart_mark_resolution_resolved();

    dmExportedSymbols();
    dmEngineInitialize();

    // argv[0] only: the engine reads game.projectc out of the mounted
    // archive, so there is no command line to pass through.
    static char  s_Arg0[] = "dmengine";
    static char* s_Argv[] = { s_Arg0, 0 };

    g_Engine = dmEngineCreate(1, s_Argv);
    if (!g_Engine)
    {
        WC_LOG("defold-wasmcart: dmEngineCreate failed");
        return;
    }
    g_EngineAlive = true;
}

__attribute__((export_name("wc_render")))
void wc_render(void)
{
    if (!g_EngineAlive)
    {
        return;
    }

    // Advance the VIRTUAL clock by one frame BEFORE the engine reads it. The
    // engine computes its frame delta from dmTime::GetMonotonicTime(), which on
    // a cart is virtual precisely so the timeline follows host stepping rather
    // than wall-clock time (see time_posix.cpp). Without this every dt-driven
    // system - timers, physics, go.animate, particle simulation - sits still
    // while rendering and input appear to work.
    dmTime::AdvanceVirtualTime(WASMCART_FRAME_US);

    // One engine tick per host frame. This is the inversion: upstream this
    // call sits inside RunLoop's while loop.
    dmEngine::UpdateResult result = (dmEngine::UpdateResult) dmEngineUpdate(g_Engine);

    if (dmEngine::RESULT_OK != result)
    {
        int run_action = 0;
        int exit_code  = 0;
        int argc       = 0;
        char** argv    = 0;
        dmEngineGetResult(g_Engine, &run_action, &exit_code, &argc, &argv);

        dmEngineDestroy(g_Engine);
        g_Engine      = 0;
        g_EngineAlive = false;
        WC_LOG("defold-wasmcart: engine exited");
    }
}

} // extern "C"
