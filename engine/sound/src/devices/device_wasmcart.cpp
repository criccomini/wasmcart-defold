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

// Sound device for a wasmcart cartridge.
//
// There is no device to open and no callback to service: the cart owns a
// stereo ring buffer in its own memory and a write cursor, and the host
// drains whatever has appeared between frames. So "queueing" is a copy into
// the ring plus a cursor bump, and "free buffer slots" is however much room
// the host's reads have left.
//
// device_js.cpp does the same job against library_sound.js; this replaces it
// so a cart imports no JS glue for audio.

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <dlib/log.h>
#include <dlib/math.h>

#include "sound.h"

namespace dmDeviceWasmcart
{
    // Stereo float frames. 8192 frames is ~170ms at 48kHz -- enough that a
    // slow frame does not underrun, small enough to stay responsive.
    static const uint32_t RING_FRAMES = 8192;

    // The ring and the cursor live here so the cart shim can publish their
    // addresses in wc_info_t. The host reads both every frame.
    float    g_AudioRing[RING_FRAMES * 2];
    uint32_t g_AudioWriteCursor = 0;

    // Set by the shim from wc_host_info_t; 0 until then.
    static uint32_t g_MixRate = 0;

    struct WasmcartDevice
    {
        // m_FreeBufferSlots is a count of BUFFERS, not frames: sound.cpp
        // loops that many times, queueing m_DeviceFrameCount frames per
        // iteration. Returning a frame count here makes the mixer queue
        // thousands of buffers in one tick and lap the ring.
        uint32_t m_BufferCount;
        uint32_t m_QueuedBuffers;
        bool     m_IsStarted;
    };

    void SetMixRate(uint32_t rate)
    {
        g_MixRate = rate;
    }

    uint32_t GetRingFrameCount()
    {
        return RING_FRAMES;
    }

    dmSound::Result DeviceWasmcartOpen(const dmSound::OpenDeviceParams* params, dmSound::HDevice* device)
    {
        assert(params);
        assert(device);

        WasmcartDevice* dev = new WasmcartDevice();
        dev->m_BufferCount    = params->m_BufferCount ? params->m_BufferCount : 4;
        dev->m_QueuedBuffers  = 0;
        dev->m_IsStarted      = false;

        memset(g_AudioRing, 0, sizeof(g_AudioRing));
        g_AudioWriteCursor = 0;

        *device = dev;

        dmLogInfo("Info");
        dmLogInfo("  nSamplesPerSec:   %u", g_MixRate ? g_MixRate : 48000);

        return dmSound::RESULT_OK;
    }

    void DeviceWasmcartClose(dmSound::HDevice device)
    {
        assert(device);
        delete (WasmcartDevice*) device;
    }

    dmSound::Result DeviceWasmcartQueue(dmSound::HDevice device, const void* samples, uint32_t sample_count)
    {
        assert(device);
        WasmcartDevice* dev = (WasmcartDevice*) device;
        if (!dev->m_IsStarted)
        {
            return dmSound::RESULT_INIT_ERROR;
        }

        // Defold only supports float output in NON-INTERLEAVED form: the
        // mixer writes the whole left channel, then the whole right
        // (sound.cpp asserts m_NonInterleavedOutput == 1 for float). The
        // wasmcart ring is interleaved stereo, so interleave on the way in.
        const float* left  = (const float*) samples;
        const float* right = left + sample_count;
        uint32_t cursor = g_AudioWriteCursor;

        for (uint32_t i = 0; i < sample_count; ++i)
        {
            const uint32_t idx = ((cursor + i) % RING_FRAMES) * 2;
            g_AudioRing[idx]     = left[i];
            g_AudioRing[idx + 1] = right[i];
        }

        // Publish only after the samples are in place: the host may read the
        // cursor at any point between frames.
        g_AudioWriteCursor = (cursor + sample_count) % RING_FRAMES;

        if (dev->m_QueuedBuffers < dev->m_BufferCount)
        {
            dev->m_QueuedBuffers++;
        }
        return dmSound::RESULT_OK;
    }

    uint32_t DeviceWasmcartFreeBufferSlots(dmSound::HDevice device)
    {
        assert(device);
        WasmcartDevice* dev = (WasmcartDevice*) device;

        // The host drains everything up to our write cursor once per frame,
        // so by the time we are asked again all previously queued buffers
        // have been taken. Retire them and report the whole pool free.
        dev->m_QueuedBuffers = 0;
        return dev->m_BufferCount;
    }

    void DeviceWasmcartDeviceInfo(dmSound::HDevice device, dmSound::DeviceInfo* info)
    {
        assert(device);
        assert(info);
        info->m_MixRate = g_MixRate ? g_MixRate : 48000;
        // The mixer hands us non-interleaved float planes; DeviceWasmcartQueue
        // interleaves them into the cart's ring, which the ABI defines as
        // interleaved stereo float32 in -1..1 (WC_FLAG_AUDIO_F32).
        info->m_UseNonInterleaved = 1;
        info->m_UseFloats         = 1;
        info->m_UseNormalized     = 1;
    }

    void DeviceWasmcartStart(dmSound::HDevice device)
    {
        assert(device);
        ((WasmcartDevice*) device)->m_IsStarted = true;
    }

    void DeviceWasmcartStop(dmSound::HDevice device)
    {
        assert(device);
        ((WasmcartDevice*) device)->m_IsStarted = false;
    }

    DM_DECLARE_SOUND_DEVICE(DefaultSoundDevice, "default", DeviceWasmcartOpen, DeviceWasmcartClose,
                            DeviceWasmcartQueue, DeviceWasmcartFreeBufferSlots, 0,
                            DeviceWasmcartDeviceInfo, DeviceWasmcartStart, DeviceWasmcartStop);
}
