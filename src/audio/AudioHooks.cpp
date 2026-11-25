// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/AudioHooks.h"

#include "core/Log.h"
#include "core/VtableHook.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace {
    // Vtable slots, fixed by the COM ABI.
    constexpr size_t c_SlotClientInitialize = 3;
    constexpr size_t c_SlotClientGetService = 14;
    constexpr size_t c_SlotRenderGetBuffer = 3;
    constexpr size_t c_SlotRenderReleaseBuffer = 4;

    using Initialize_t = HRESULT(STDMETHODCALLTYPE*)(
        IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID
    );
    using GetService_t = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
    using GetBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
    using ReleaseBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);

    Initialize_t g_Initialize = nullptr;
    GetService_t g_GetService = nullptr;
    GetBuffer_t g_GetBuffer = nullptr;
    ReleaseBuffer_t g_ReleaseBuffer = nullptr;

    void** g_ClientVtable = nullptr;
    void** g_RenderVtable = nullptr;

    struct RenderStream {
        AudioHooks::ChannelLayout m_Layout;
        BYTE* m_Buffer = nullptr;
    };

    std::mutex g_Mutex;
    std::unordered_map<IAudioClient*, AudioHooks::ChannelLayout> g_Clients;
    std::unordered_map<IAudioRenderClient*, RenderStream> g_Streams;

    // For streams opened before the hooks went in, whose format was never seen.
    AudioHooks::ChannelLayout g_DefaultLayout;

    std::atomic<bool> g_Enabled = false;
    std::atomic<uint64_t> g_FramesSwapped = 0;
    std::atomic<uint64_t> g_Buffers = 0;
    std::atomic<uint16_t> g_LastChannels = 0;
    std::atomic<uint16_t> g_LastPairs = 0;

    Log::Budget g_FormatLines(4);

    HRESULT STDMETHODCALLTYPE InitializeDetour(
        IAudioClient* p_Client, AUDCLNT_SHAREMODE p_ShareMode, DWORD p_Flags, REFERENCE_TIME p_Duration,
        REFERENCE_TIME p_Periodicity, const WAVEFORMATEX* p_Format, LPCGUID p_Session
    ) {
        const HRESULT s_Result = g_Initialize(p_Client, p_ShareMode, p_Flags, p_Duration, p_Periodicity, p_Format, p_Session);

        if (SUCCEEDED(s_Result)) {
            const AudioHooks::ChannelLayout s_Layout = AudioHooks::LayoutFromFormat(p_Format);

            if (s_Layout.m_NumPairs == 0 && p_Format && g_FormatLines.Take()) {
                Log::Warning(
                    "audio", "A stream with {} channels and format tag {:#x} has no channels to swap, so it stays unchanged.", p_Format->nChannels,
                    p_Format->wFormatTag
                );
            }

            std::scoped_lock s_Lock(g_Mutex);
            g_Clients[p_Client] = s_Layout;
        }

        return s_Result;
    }

    HRESULT STDMETHODCALLTYPE GetServiceDetour(IAudioClient* p_Client, REFIID p_Iid, void** p_Service) {
        const HRESULT s_Result = g_GetService(p_Client, p_Iid, p_Service);

        if (SUCCEEDED(s_Result) && p_Iid == __uuidof(IAudioRenderClient)) {
            std::scoped_lock s_Lock(g_Mutex);
            const auto s_Client = g_Clients.find(p_Client);
            RenderStream s_Stream;
            s_Stream.m_Layout = s_Client != g_Clients.end() ? s_Client->second : g_DefaultLayout;
            g_Streams[static_cast<IAudioRenderClient*>(*p_Service)] = s_Stream;
        }

        return s_Result;
    }

    HRESULT STDMETHODCALLTYPE GetBufferDetour(IAudioRenderClient* p_Render, UINT32 p_Frames, BYTE** p_Data) {
        const HRESULT s_Result = g_GetBuffer(p_Render, p_Frames, p_Data);

        if (SUCCEEDED(s_Result)) {
            std::scoped_lock s_Lock(g_Mutex);
            const auto [s_It, s_New] = g_Streams.try_emplace(p_Render);

            if (s_New) {
                s_It->second.m_Layout = g_DefaultLayout;
            }

            s_It->second.m_Buffer = *p_Data;
        }

        return s_Result;
    }

    HRESULT STDMETHODCALLTYPE ReleaseBufferDetour(IAudioRenderClient* p_Render, UINT32 p_Frames, DWORD p_Flags) {
        BYTE* s_Buffer = nullptr;
        AudioHooks::ChannelLayout s_Layout;

        {
            std::scoped_lock s_Lock(g_Mutex);
            const auto s_It = g_Streams.find(p_Render);

            if (s_It != g_Streams.end()) {
                s_Buffer = s_It->second.m_Buffer;
                s_Layout = s_It->second.m_Layout;
                s_It->second.m_Buffer = nullptr;
            }
        }

        // With the silent flag the audio engine ignores the buffer contents.
        if (g_Enabled.load(std::memory_order_relaxed) && s_Buffer && !(p_Flags & AUDCLNT_BUFFERFLAGS_SILENT) &&
            s_Layout.m_NumPairs > 0) {
            AudioHooks::SwapChannels(s_Buffer, p_Frames, s_Layout);
            g_FramesSwapped.fetch_add(p_Frames, std::memory_order_relaxed);
            g_Buffers.fetch_add(1, std::memory_order_relaxed);
            g_LastChannels.store(s_Layout.m_Channels, std::memory_order_relaxed);
            g_LastPairs.store(s_Layout.m_NumPairs, std::memory_order_relaxed);
        }

        return g_ReleaseBuffer(p_Render, p_Frames, p_Flags);
    }

    const VtableHook::Slot c_ClientSlots[] = {
        { c_SlotClientInitialize, reinterpret_cast<void*>(&InitializeDetour), reinterpret_cast<void**>(&g_Initialize) },
        { c_SlotClientGetService, reinterpret_cast<void*>(&GetServiceDetour), reinterpret_cast<void**>(&g_GetService) },
    };

    const VtableHook::Slot c_RenderSlots[] = {
        { c_SlotRenderGetBuffer, reinterpret_cast<void*>(&GetBufferDetour), reinterpret_cast<void**>(&g_GetBuffer) },
        { c_SlotRenderReleaseBuffer, reinterpret_cast<void*>(&ReleaseBufferDetour), reinterpret_cast<void**>(&g_ReleaseBuffer) },
    };

    uint32_t DefaultChannelMask(uint16_t p_Channels) {
        switch (p_Channels) {
            case 2: return KSAUDIO_SPEAKER_STEREO;
            case 4: return KSAUDIO_SPEAKER_QUAD;
            case 6: return KSAUDIO_SPEAKER_5POINT1;
            case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
            default: return 0;
        }
    }

    bool InstallOnThisThread(std::string* p_Error) {
        const auto s_Fail = [&](const char* p_Step, HRESULT p_Result) {
            if (p_Error) {
                *p_Error = std::format("{} failed with {:#x}", p_Step, static_cast<uint32_t>(p_Result));
            }
            return false;
        };

        ComPtr<IMMDeviceEnumerator> s_Enumerator;
        HRESULT s_Result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&s_Enumerator)
        );
        if (FAILED(s_Result)) {
            return s_Fail("CoCreateInstance(MMDeviceEnumerator)", s_Result);
        }

        ComPtr<IMMDevice> s_Device;
        s_Result = s_Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s_Device);
        if (FAILED(s_Result)) {
            return s_Fail("GetDefaultAudioEndpoint", s_Result);
        }

        ComPtr<IAudioClient> s_Client;
        s_Result = s_Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s_Client.GetAddressOf()));
        if (FAILED(s_Result)) {
            return s_Fail("Activate(IAudioClient)", s_Result);
        }

        WAVEFORMATEX* s_MixFormat = nullptr;
        s_Result = s_Client->GetMixFormat(&s_MixFormat);
        if (FAILED(s_Result)) {
            return s_Fail("GetMixFormat", s_Result);
        }

        // A render client only exists on an initialized stream. It is never started, so it plays nothing.
        constexpr REFERENCE_TIME c_OneSecond = 10'000'000;
        s_Result = s_Client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, c_OneSecond, 0, s_MixFormat, nullptr);
        g_DefaultLayout = AudioHooks::LayoutFromFormat(s_MixFormat);
        CoTaskMemFree(s_MixFormat);

        if (FAILED(s_Result)) {
            return s_Fail("IAudioClient::Initialize", s_Result);
        }

        ComPtr<IAudioRenderClient> s_Render;
        s_Result = s_Client->GetService(IID_PPV_ARGS(&s_Render));
        if (FAILED(s_Result)) {
            return s_Fail("GetService(IAudioRenderClient)", s_Result);
        }

        void** s_ClientVtable = *reinterpret_cast<void***>(s_Client.Get());
        void** s_RenderVtable = *reinterpret_cast<void***>(s_Render.Get());

        if (!VtableHook::Install(s_ClientVtable, c_ClientSlots)) {
            return s_Fail("Patching the IAudioClient vtable", E_FAIL);
        }

        if (!VtableHook::Install(s_RenderVtable, c_RenderSlots)) {
            VtableHook::Remove(s_ClientVtable, c_ClientSlots);
            return s_Fail("Patching the IAudioRenderClient vtable", E_FAIL);
        }

        g_ClientVtable = s_ClientVtable;
        g_RenderVtable = s_RenderVtable;
        return true;
    }
}

AudioHooks::ChannelLayout AudioHooks::LayoutFromFormat(const WAVEFORMATEX* p_Format) {
    ChannelLayout s_Layout;

    if (!p_Format || p_Format->nChannels == 0 || p_Format->nBlockAlign == 0) {
        return s_Layout;
    }

    uint32_t s_Mask = 0;

    if (p_Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* s_Extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(p_Format);

        if (s_Extensible->SubFormat != KSDATAFORMAT_SUBTYPE_PCM &&
            s_Extensible->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            return s_Layout;
        }

        s_Mask = s_Extensible->dwChannelMask;
    }
    else if (p_Format->wFormatTag != WAVE_FORMAT_PCM && p_Format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
        return s_Layout;
    }

    if (s_Mask == 0) {
        s_Mask = DefaultChannelMask(p_Format->nChannels);
    }

    s_Layout.m_BlockAlign = p_Format->nBlockAlign;
    s_Layout.m_Channels = p_Format->nChannels;
    s_Layout.m_BytesPerSample = p_Format->nBlockAlign / p_Format->nChannels;

    static constexpr uint32_t c_Pairs[][2] = {
        { SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT },
        { SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT },
        { SPEAKER_FRONT_LEFT_OF_CENTER, SPEAKER_FRONT_RIGHT_OF_CENTER },
        { SPEAKER_SIDE_LEFT, SPEAKER_SIDE_RIGHT },
    };

    // Channels are stored in the order of their bits in the mask.
    const auto s_IndexOf = [&](uint32_t p_Bit) { return std::popcount(s_Mask & (p_Bit - 1)); };

    for (const auto& s_Pair : c_Pairs) {
        if ((s_Mask & s_Pair[0]) && (s_Mask & s_Pair[1]) &&
            s_IndexOf(s_Pair[0]) < p_Format->nChannels && s_IndexOf(s_Pair[1]) < p_Format->nChannels) {
            s_Layout.m_Pairs[s_Layout.m_NumPairs][0] = static_cast<uint16_t>(s_IndexOf(s_Pair[0]));
            s_Layout.m_Pairs[s_Layout.m_NumPairs][1] = static_cast<uint16_t>(s_IndexOf(s_Pair[1]));
            ++s_Layout.m_NumPairs;
        }
    }

    return s_Layout;
}

void AudioHooks::SwapChannels(uint8_t* p_Data, uint32_t p_Frames, const ChannelLayout& p_Layout) {
    for (uint32_t s_Frame = 0; s_Frame < p_Frames; ++s_Frame) {
        uint8_t* s_Samples = p_Data + static_cast<size_t>(s_Frame) * p_Layout.m_BlockAlign;

        for (uint16_t s_Pair = 0; s_Pair < p_Layout.m_NumPairs; ++s_Pair) {
            uint8_t* s_A = s_Samples + static_cast<size_t>(p_Layout.m_Pairs[s_Pair][0]) * p_Layout.m_BytesPerSample;
            uint8_t* s_B = s_Samples + static_cast<size_t>(p_Layout.m_Pairs[s_Pair][1]) * p_Layout.m_BytesPerSample;
            std::swap_ranges(s_A, s_A + p_Layout.m_BytesPerSample, s_B);
        }
    }
}

bool AudioHooks::Install(std::string* p_Error) {
    if (g_ClientVtable) {
        return true;
    }

    // On a thread of its own so that the caller's COM apartment stays as the game set it up.
    bool s_Installed = false;
    std::thread s_Thread([&] {
        const HRESULT s_Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        s_Installed = InstallOnThisThread(p_Error);

        if (SUCCEEDED(s_Com)) {
            CoUninitialize();
        }
    });
    s_Thread.join();

    return s_Installed;
}

void AudioHooks::Uninstall() {
    if (!g_ClientVtable) {
        return;
    }

    g_Enabled = false;

    VtableHook::Remove(g_ClientVtable, c_ClientSlots);
    VtableHook::Remove(g_RenderVtable, c_RenderSlots);

    g_ClientVtable = nullptr;
    g_RenderVtable = nullptr;

    std::scoped_lock s_Lock(g_Mutex);
    g_Clients.clear();
    g_Streams.clear();
}

void AudioHooks::SetEnabled(bool p_Enabled) {
    g_Enabled = p_Enabled;
}

AudioHooks::Stats AudioHooks::GetStats() {
    Stats s_Stats;
    s_Stats.m_FramesSwapped = g_FramesSwapped.load(std::memory_order_relaxed);
    s_Stats.m_Buffers = g_Buffers.load(std::memory_order_relaxed);
    s_Stats.m_Channels = g_LastChannels.load(std::memory_order_relaxed);
    s_Stats.m_NumPairs = g_LastPairs.load(std::memory_order_relaxed);

    std::scoped_lock s_Lock(g_Mutex);
    s_Stats.m_Streams = static_cast<uint32_t>(g_Streams.size());
    return s_Stats;
}
