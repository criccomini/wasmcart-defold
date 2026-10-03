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
#include "engine_version.h"

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
namespace dmEngine
{
    extern uint64_t g_WcPhaseHid;
    extern uint64_t g_WcPhaseExt;
    extern uint64_t g_WcPhaseUpdate;
    extern uint64_t g_WcPhaseSound;
    extern uint64_t g_WcPhaseRender;
    extern uint64_t g_WcPhaseDraw;
    extern uint64_t g_WcPhasePost;
    extern uint64_t g_WcPhaseFlip;
}

namespace dmGraphics
{
    extern uint32_t g_WcDrawCalls;
    extern uint32_t g_WcTriangles;
    extern uint32_t g_WcFlips;
}

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
// Fallback frame length, used only for the very first frame and if a host ever
// reports a non-positive delta. Real pacing comes from wc_time_t.delta_ms,
// which the host writes before every wc_render.
#ifndef WASMCART_BUILD
#define WASMCART_BUILD "unstamped"
#endif
#ifndef WASMCART_VERSION
#define WASMCART_VERSION "0.0.0"
#endif

#define WASMCART_FRAME_US (1000000 / 60)

// The host already clamps delta_ms (the reference hosts cap it at 250 ms), but
// a cart cannot assume that of every host, so clamp again here: a stall that
// reaches the simulation moves a frame's worth of velocity in one step and
// tunnels straight through collisions.
#define WASMCART_MAX_FRAME_US (250 * 1000)

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

    // Stamp the build into the cart's own log. The engine prints a Defold
    // version too, but that sha is captured when CMake configures and goes
    // stale across incremental builds, so it can name a commit the binary does
    // not contain. WASMCART_BUILD is injected per compile, which makes it the
    // figure to trust when checking which cart you are actually running.
    {
        char buf[192];
        int n = snprintf(buf, sizeof(buf),
            "wasmcart-defold %s build %s (defold %s)",
            WASMCART_VERSION, WASMCART_BUILD, dmEngineVersion::VERSION);
        if (n > 0) wc_log(buf, (unsigned int) n);
    }
}

__attribute__((export_name("wc_render")))
void wc_render(void)
{
    if (!g_EngineAlive)
    {
        return;
    }

    // Advance the VIRTUAL clock BEFORE the engine reads it. The engine computes
    // its frame delta from dmTime::GetMonotonicTime(), which on a cart is
    // virtual precisely so the timeline follows host stepping rather than
    // wall-clock time (see time_posix.cpp). Without this every dt-driven system
    // - timers, physics, go.animate, particle simulation - sits still while
    // rendering and input appear to work.
    //
    // The step is the host's own delta_ms, not a fixed 1/60. A hardcoded step
    // makes the cart run at half speed on a 120 Hz host and at double speed on
    // a 30 Hz one, because the simulated time per call no longer matches the
    // rate the host actually calls at. wc_time_t is the ABI's clock and the
    // spec requires a cart to take its timing from it.
    uint64_t frame_us = WASMCART_FRAME_US;
    if (g_Time.delta_ms > 0.0)
    {
        frame_us = (uint64_t)(g_Time.delta_ms * 1000.0);
        if (frame_us > WASMCART_MAX_FRAME_US)
        {
            frame_us = WASMCART_MAX_FRAME_US;
        }
        if (frame_us == 0)
        {
            // Sub-microsecond frame: still advance, or a fast host freezes the
            // timeline entirely by always rounding down to zero.
            frame_us = 1;
        }
    }
    dmTime::AdvanceVirtualTime(frame_us);


    // One engine tick per host frame. This is the inversion: upstream this
    // call sits inside RunLoop's while loop.
    // Time the engine pass itself, so the report can say whether a slow frame
    // is the cart working hard or the host simply not calling.
    const uint64_t update_t0 = dmTime::GetTime();
    dmEngine::UpdateResult result = (dmEngine::UpdateResult) dmEngineUpdate(g_Engine);
    const uint64_t update_us = dmTime::GetTime() - update_t0;



    // Keep the audio ring honest.
    //
    // wc_info_t advertises audio_cap, which tells the host this cart produces
    // audio. Defold only runs its mixer while something is playing (DEF-3130
    // does not even start the device otherwise), so between sounds the write
    // cursor stands still while the host keeps draining -- the cart claims a
    // stream it is not supplying.
    //
    // A host that paces on audio then sees a ring that never fills. The node
    // player stepped five frames per presented one because of exactly this,
    // discarding four frames in five. Write silence for the frames the mixer
    // did not cover, so the cursor advances at the rate the declaration
    // promises whether or not a sound happens to be playing.
    uint64_t audio_us = 0;
    uint32_t audio_written = 0;
    {
        const uint64_t audio_t0 = dmTime::GetTime();
        const uint32_t rate   = g_HostInfo.audio_sample_rate ? g_HostInfo.audio_sample_rate : 48000;
        const uint32_t ring   = dmDeviceWasmcart::GetRingFrameCount();
        // Exactly the audio this frame covers, scaled by the delta the host
        // handed over. A fixed 1/60 s per call looks right only while the host
        // calls at 60 Hz: call twice as often and the cart emits audio at twice
        // realtime, the device cannot drain it, and a host that throttles on a
        // full audio queue then starves the cart of frames. That is not
        // hypothetical -- it pinned every example at 11.7 fps.
        //
        // The device drains in realtime, so the cart must produce in realtime.
        uint32_t want = (uint32_t)(((uint64_t) rate * frame_us) / 1000000u);
        // A host stepping far faster than realtime (a headless harness) yields
        // a sub-sample delta. Emitting nothing is correct there: the ring is
        // not a heartbeat, it is a stream, and there is no stream to supply.
        if (want > ring) want = ring;
        static uint32_t s_LastCursor = 0;
        uint32_t cursor = dmDeviceWasmcart::g_AudioWriteCursor;
        uint32_t wrote  = (cursor - s_LastCursor) % ring;
        if (wrote < want)
        {
            uint32_t pad = want - wrote;
            for (uint32_t i = 0; i < pad; ++i)
            {
                const uint32_t idx = ((cursor + i) % ring) * 2;
                dmDeviceWasmcart::g_AudioRing[idx]     = 0.0f;
                dmDeviceWasmcart::g_AudioRing[idx + 1] = 0.0f;
            }
            cursor = (cursor + pad) % ring;
            dmDeviceWasmcart::g_AudioWriteCursor = cursor;
        }
        audio_written = (uint32_t)((cursor - s_LastCursor) % ring);
        s_LastCursor = cursor;
        audio_us = dmTime::GetTime() - audio_t0;
    }

    // Per-second report of everything the cart can see about its own frame.
    //
    // A cart cannot time the host's blit or its vsync wait, but it can time
    // every phase on its own side and, crucially, the GAP: the wall-clock time
    // between one wc_render returning and the next one starting. That gap is
    // all host: present, swap, event loop, audio queue, anything else. If the
    // cart is fast and the gap is large, the cart is not the problem, and the
    // size and shape of the gap says which part of the host to look at.
    //
    // Keyed off the host's wall clock (wc_time_t.time_ms), so a second here is
    // a real second.
    {
        static double   s_WallStartMs  = -1.0;
        static double   s_PrevWallMs   = -1.0;
        static double   s_SimAccumMs   = 0.0;
        static uint32_t s_Frames       = 0;
        static uint64_t s_UpdateUsSum  = 0;
        static uint64_t s_UpdateUsMax  = 0;
        static uint64_t s_AudioUsSum   = 0;
        static double   s_DeltaMsMin   = 1e9;
        static double   s_DeltaMsMax   = 0.0;
        static double   s_GapMsSum     = 0.0;
        static double   s_GapMsMax     = 0.0;
        static uint32_t s_GapOver33    = 0;
        static uint32_t s_GapOver100   = 0;
        static uint32_t s_AudioFrames  = 0;
        static uint32_t s_AudioSamples = 0;
        static uint32_t s_Draws        = 0;
        static uint32_t s_Tris         = 0;
        static uint32_t s_Flips        = 0;
        static uint32_t s_NoFlipFrames = 0;
        static uint64_t s_PHid=0, s_PExt=0, s_PUpd=0, s_PSnd=0, s_PRen=0, s_PDrw=0, s_PPost=0, s_PFlip=0;

        const double wall_ms = g_Time.time_ms;
        if (s_WallStartMs < 0.0) s_WallStartMs = wall_ms;

        // Gap = wall time since the END of the previous wc_render. Everything
        // the host did between frames lands here.
        if (s_PrevWallMs >= 0.0)
        {
            const double gap = wall_ms - s_PrevWallMs;
            s_GapMsSum += gap;
            if (gap > s_GapMsMax) s_GapMsMax = gap;
            if (gap > 33.0)  s_GapOver33++;
            if (gap > 100.0) s_GapOver100++;
        }

        const double delta_ms = (double) frame_us / 1000.0;
        s_Frames       += 1;
        s_SimAccumMs   += delta_ms;
        s_UpdateUsSum  += update_us;
        s_AudioUsSum   += audio_us;
        s_AudioSamples += audio_written;
        if (audio_written) s_AudioFrames++;
        s_Draws += dmGraphics::g_WcDrawCalls;
        s_Tris  += dmGraphics::g_WcTriangles;
        s_Flips += dmGraphics::g_WcFlips;
        if (dmGraphics::g_WcFlips == 0) s_NoFlipFrames++;
        dmGraphics::g_WcDrawCalls = 0;
        dmGraphics::g_WcTriangles = 0;
        dmGraphics::g_WcFlips     = 0;
        s_PHid += dmEngine::g_WcPhaseHid;   s_PExt  += dmEngine::g_WcPhaseExt;
        s_PUpd += dmEngine::g_WcPhaseUpdate;s_PSnd  += dmEngine::g_WcPhaseSound;
        s_PRen += dmEngine::g_WcPhaseRender;s_PDrw  += dmEngine::g_WcPhaseDraw;
        s_PPost+= dmEngine::g_WcPhasePost;  s_PFlip += dmEngine::g_WcPhaseFlip;
        dmEngine::g_WcPhaseHid=0; dmEngine::g_WcPhaseExt=0; dmEngine::g_WcPhaseUpdate=0;
        dmEngine::g_WcPhaseSound=0; dmEngine::g_WcPhaseRender=0; dmEngine::g_WcPhaseDraw=0;
        dmEngine::g_WcPhasePost=0; dmEngine::g_WcPhaseFlip=0;
        if (update_us > s_UpdateUsMax) s_UpdateUsMax = update_us;
        if (delta_ms < s_DeltaMsMin)   s_DeltaMsMin  = delta_ms;
        if (delta_ms > s_DeltaMsMax)   s_DeltaMsMax  = delta_ms;

        const double wall_elapsed = wall_ms - s_WallStartMs;
        if (wall_elapsed >= 1000.0)
        {
            const double fn      = (double) s_Frames;
            const double fps     = fn * 1000.0 / wall_elapsed;
            const double avg_up  = (double) s_UpdateUsSum / fn / 1000.0;
            const double avg_au  = (double) s_AudioUsSum / fn / 1000.0;
            const double cart_ms = (double)(s_UpdateUsSum + s_AudioUsSum) / 1000.0;
            const double gap_ms  = s_GapMsSum;
            char buf[512];
            int n = snprintf(buf, sizeof(buf),
                "wasmcart: %.1f fps | %u frames in %.0f ms real / %.0f ms simulated\n"
                "  delta   min %.1f avg %.1f max %.1f ms\n"
                "  update  avg %.3f max %.3f ms | audio avg %.3f ms\n"
                "  IN CART %.0f ms of %.0f ms (%.0f%%)\n"
                "  GAP     avg %.1f max %.1f ms, %.0f%% of the second, %u over 33 ms, %u over 100 ms\n"
                "  render  %u draws, %u tris, %u flips | %u frames rendered NOTHING\n"
                "  phases  hid %.1f ext %.1f update %.1f sound %.1f ms\n"
                "          render %.1f draw %.1f post %.1f FLIP %.1f ms\n"
                "  audio   %u/%u frames wrote %u samples (%.2fx realtime)",
                fps, s_Frames, wall_elapsed, s_SimAccumMs,
                s_DeltaMsMin, s_SimAccumMs / fn, s_DeltaMsMax,
                avg_up, (double) s_UpdateUsMax / 1000.0, avg_au,
                cart_ms, wall_elapsed, cart_ms * 100.0 / wall_elapsed,
                gap_ms / fn, s_GapMsMax, gap_ms * 100.0 / wall_elapsed,
                s_GapOver33, s_GapOver100,
                s_Draws, s_Tris, s_Flips, s_NoFlipFrames,
                s_PHid/1000.0, s_PExt/1000.0, s_PUpd/1000.0, s_PSnd/1000.0,
                s_PRen/1000.0, s_PDrw/1000.0, s_PPost/1000.0, s_PFlip/1000.0,
                s_AudioFrames, s_Frames, s_AudioSamples,
                ((double) s_AudioSamples / (double)(g_HostInfo.audio_sample_rate ? g_HostInfo.audio_sample_rate : 48000))
                    / (wall_elapsed / 1000.0));
            if (n > 0) wc_log(buf, (unsigned int) n);

            s_WallStartMs  = wall_ms;
            s_SimAccumMs   = 0.0;
            s_Frames       = 0;
            s_UpdateUsSum  = 0;
            s_UpdateUsMax  = 0;
            s_AudioUsSum   = 0;
            s_DeltaMsMin   = 1e9;
            s_DeltaMsMax   = 0.0;
            s_GapMsSum     = 0.0;
            s_GapMsMax     = 0.0;
            s_GapOver33    = 0;
            s_GapOver100   = 0;
            s_AudioFrames  = 0;
            s_AudioSamples = 0;
            s_Draws        = 0;
            s_Tris         = 0;
            s_Flips        = 0;
            s_NoFlipFrames = 0;
            s_PHid=0; s_PExt=0; s_PUpd=0; s_PSnd=0; s_PRen=0; s_PDrw=0; s_PPost=0; s_PFlip=0;
        }
        s_PrevWallMs = wall_ms;
    }

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
