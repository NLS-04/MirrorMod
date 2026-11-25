// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone check of the channel swap, including the hooks on a real WASAPI stream.
// The stream is never started, so this plays nothing.

#include "TestSupport.h"

#include "audio/AudioHooks.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
    WAVEFORMATEXTENSIBLE Extensible(uint16_t p_Channels, uint16_t p_ContainerBits, uint16_t p_ValidBits, uint32_t p_Mask, const GUID& p_SubFormat) {
        WAVEFORMATEXTENSIBLE s_Format {};
        s_Format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        s_Format.Format.nChannels = p_Channels;
        s_Format.Format.nSamplesPerSec = 48000;
        s_Format.Format.wBitsPerSample = p_ContainerBits;
        s_Format.Format.nBlockAlign = static_cast<WORD>(p_Channels * p_ContainerBits / 8);
        s_Format.Format.nAvgBytesPerSec = 48000 * s_Format.Format.nBlockAlign;
        s_Format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        s_Format.Samples.wValidBitsPerSample = p_ValidBits;
        s_Format.dwChannelMask = p_Mask;
        s_Format.SubFormat = p_SubFormat;
        return s_Format;
    }

    // Each channel of each frame gets a unique value, so a wrong swap or a wrong stride is detected.
    template <class T>
    std::vector<T> Pattern(uint32_t p_Frames, uint16_t p_Channels) {
        std::vector<T> s_Data(static_cast<size_t>(p_Frames) * p_Channels);
        for (uint32_t f = 0; f < p_Frames; ++f) {
            for (uint16_t c = 0; c < p_Channels; ++c) {
                s_Data[static_cast<size_t>(f) * p_Channels + c] = static_cast<T>(f * 100 + c + 1);
            }
        }
        return s_Data;
    }

    template <class T>
    bool Matches(const std::vector<T>& p_Data, uint32_t p_Frames, uint16_t p_Channels, const std::vector<uint16_t>& p_ExpectedOrder) {
        for (uint32_t f = 0; f < p_Frames; ++f) {
            for (uint16_t c = 0; c < p_Channels; ++c) {
                if (p_Data[static_cast<size_t>(f) * p_Channels + c] != static_cast<T>(f * 100 + p_ExpectedOrder[c] + 1)) {
                    return false;
                }
            }
        }
        return true;
    }

    template <class T>
    void TestSwap(const WAVEFORMATEX* p_Format, const std::vector<uint16_t>& p_Expected, const char* p_Name) {
        const auto s_Layout = AudioHooks::LayoutFromFormat(p_Format);
        auto s_Data = Pattern<T>(512, p_Format->nChannels);
        AudioHooks::SwapChannels(reinterpret_cast<uint8_t*>(s_Data.data()), 512, s_Layout);
        Check(Matches(s_Data, 512, p_Format->nChannels, p_Expected), p_Name);
    }

    void TestLayouts() {
        WAVEFORMATEX s_Float {};
        s_Float.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        s_Float.nChannels = 2;
        s_Float.wBitsPerSample = 32;
        s_Float.nBlockAlign = 8;
        TestSwap<float>(&s_Float, { 1, 0 }, "stereo float");

        WAVEFORMATEX s_Pcm16 {};
        s_Pcm16.wFormatTag = WAVE_FORMAT_PCM;
        s_Pcm16.nChannels = 2;
        s_Pcm16.wBitsPerSample = 16;
        s_Pcm16.nBlockAlign = 4;
        TestSwap<int16_t>(&s_Pcm16, { 1, 0 }, "stereo 16 bit");

        const auto s_Stereo24In32 = Extensible(2, 32, 24, KSAUDIO_SPEAKER_STEREO, KSDATAFORMAT_SUBTYPE_PCM);
        TestSwap<int32_t>(&s_Stereo24In32.Format, { 1, 0 }, "stereo 24 in 32 bit extensible");

        const auto s_51 = Extensible(6, 32, 32, KSAUDIO_SPEAKER_5POINT1, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        TestSwap<float>(&s_51.Format, { 1, 0, 2, 3, 5, 4 }, "5.1: front and back swapped, center and LFE kept");

        const auto s_51Surround = Extensible(6, 32, 32, KSAUDIO_SPEAKER_5POINT1_SURROUND, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        TestSwap<float>(&s_51Surround.Format, { 1, 0, 2, 3, 5, 4 }, "5.1 surround: front and side swapped");

        const auto s_71 = Extensible(8, 32, 32, KSAUDIO_SPEAKER_7POINT1_SURROUND, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        TestSwap<float>(&s_71.Format, { 1, 0, 2, 3, 5, 4, 7, 6 }, "7.1: front, back and side swapped");

        const auto s_NoMask = Extensible(2, 32, 32, 0, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        TestSwap<float>(&s_NoMask.Format, { 1, 0 }, "stereo without a channel mask");

        WAVEFORMATEX s_Mono = s_Float;
        s_Mono.nChannels = 1;
        s_Mono.nBlockAlign = 4;
        TestSwap<float>(&s_Mono, { 0 }, "mono stays untouched");

        const auto s_Bitstream = Extensible(2, 16, 16, KSAUDIO_SPEAKER_STEREO, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL);
        Check(AudioHooks::LayoutFromFormat(&s_Bitstream.Format).m_NumPairs == 0, "compressed bitstream stays untouched");
    }

    void TestRealStream() {
        std::string s_Error;
        const bool s_Installed = AudioHooks::Install(&s_Error);
        printf("[INFO] Install: %s\n", s_Installed ? "ok" : s_Error.c_str());
        Check(s_Installed, "hooks install on the default playback device");
        if (!s_Installed) {
            return;
        }

        ComPtr<IMMDeviceEnumerator> s_Enumerator;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&s_Enumerator));
        ComPtr<IMMDevice> s_Device;
        s_Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s_Device);
        ComPtr<IAudioClient> s_Client;
        s_Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s_Client.GetAddressOf()));

        WAVEFORMATEX* s_Format = nullptr;
        s_Client->GetMixFormat(&s_Format);
        printf("[INFO] Mix format: %u channels, %u bit, tag 0x%X\n", s_Format->nChannels, s_Format->wBitsPerSample, s_Format->wFormatTag);

        Check(SUCCEEDED(s_Client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, s_Format, nullptr)), "stream initializes through the hook");

        ComPtr<IAudioRenderClient> s_Render;
        Check(SUCCEEDED(s_Client->GetService(IID_PPV_ARGS(&s_Render))), "render client through the hook");

        const auto s_Layout = AudioHooks::LayoutFromFormat(s_Format);
        constexpr UINT32 c_Frames = 256;

        // Writes L = 1, R = 2, ... into the stream's real buffer and returns what is there after the release.
        const auto s_Round = [&](DWORD p_Flags) {
            BYTE* s_Data = nullptr;
            s_Render->GetBuffer(c_Frames, &s_Data);
            for (UINT32 f = 0; f < c_Frames; ++f) {
                for (uint16_t c = 0; c < s_Format->nChannels; ++c) {
                    memset(s_Data + f * s_Format->nBlockAlign + c * s_Layout.m_BytesPerSample, c + 1, s_Layout.m_BytesPerSample);
                }
            }
            s_Render->ReleaseBuffer(c_Frames, p_Flags);
            return std::vector<BYTE>(s_Data, s_Data + s_Layout.m_BytesPerSample * 2);
        };

        const auto s_FirstValue = [&](const std::vector<BYTE>& p_Bytes) { return static_cast<int>(p_Bytes[0]); };
        const auto s_SecondValue = [&](const std::vector<BYTE>& p_Bytes) { return static_cast<int>(p_Bytes[s_Layout.m_BytesPerSample]); };

        AudioHooks::SetEnabled(false);
        const auto s_Off = s_Round(0);
        Check(s_FirstValue(s_Off) == 1 && s_SecondValue(s_Off) == 2 && AudioHooks::GetStats().m_FramesSwapped == 0, "disabled: stream is left alone");

        AudioHooks::SetEnabled(true);
        const auto s_On = s_Round(0);
        Check(s_FirstValue(s_On) == 2 && s_SecondValue(s_On) == 1, "enabled: left and right arrive swapped in the real buffer");
        Check(AudioHooks::GetStats().m_FramesSwapped == c_Frames, "frames are counted");

        const auto s_Silent = s_Round(AUDCLNT_BUFFERFLAGS_SILENT);
        Check(s_FirstValue(s_Silent) == 1 && AudioHooks::GetStats().m_FramesSwapped == c_Frames, "silent buffers are skipped");

        const auto s_Stats = AudioHooks::GetStats();
        printf("[INFO] Streams %u, channels %u, swapped pairs %u\n", s_Stats.m_Streams, s_Stats.m_Channels, s_Stats.m_NumPairs);

        s_Client->Reset();
        CoTaskMemFree(s_Format);

        AudioHooks::Uninstall();

        s_Render.Reset();
        s_Client.Reset();
        Check(true, "hooks removed cleanly");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    TestLayouts();
    TestRealStream();

    return Finish();
}
