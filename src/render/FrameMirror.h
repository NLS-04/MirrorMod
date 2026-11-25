// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "render/MirrorFlip.h"

#include <Glacier/ZRender.h>

#include <atomic>
#include <string>

#include <dxgi1_4.h>
#include <wrl/client.h>

class SceneState;
class Settings;

/**
 * Decides where each frame gets flipped, and does it.
 *
 * Normally the flip is recorded into the engine's own command list, right before the first UI draw of the frame, so the
 * UI stays readable. A frame that never reaches that point has no UI to protect and is flipped as a whole at present.
 *
 * Everything here runs on the render thread.
 */
class FrameMirror {
public:
    struct Stats {
        uint64_t m_FlipsBeforeUi = 0;
        uint64_t m_FlipsAtPresent = 0;
        uint64_t m_FlipsAborted = 0;
        uint64_t m_TargetsRejected = 0;
    };

    FrameMirror(const Settings& p_Settings, SceneState& p_Scene);
    ~FrameMirror();

    /** Call right before the engine draws a UI pass. Returns whether the flip is armed, for AfterUiPass. */
    bool BeforeUiPass(ZRenderTargetView** p_Views, bool p_CaptureOnly);
    void AfterUiPass(bool p_Armed);

    void Shutdown();

    /** Writes one line per frame to the log for the next p_Frames frames. */
    void TraceFrames(int p_Frames);

    Stats GetStats() const;

    /** Empty unless the renderer could not be set up. */
    const std::string& GetError() const { return m_Error; }

    /** Set after the flip failed its rehearsal, which leaves only the whole frame fallback. */
    bool IsEnginePathDisabled() const { return m_EnginePathDisabled; }

private:
    static constexpr UINT c_FrameContexts = 4;

    struct PresentFrame {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Allocator;
        uint64_t m_FenceValue = 0;
    };

    static void OnArmedSite(
        void* p_Context, ID3D12GraphicsCommandList* p_List, ID3D12PipelineState* p_Pipeline, ID3D12RootSignature* p_ComputeRootSignature
    );
    static void OnPresent(void* p_Context, IDXGISwapChain* p_SwapChain);
    static void OnViewport(void* p_Context, const D3D12_VIEWPORT& p_Viewport);
    static void OnCloseFailed(void* p_Context, ID3D12GraphicsCommandList* p_List, HRESULT p_Result);

    bool EnsureReady();
    bool CreatePresentResources(ID3D12Device* p_Device, std::string* p_Error);
    ID3D12Resource* ResolveFlipTarget(ZRenderTargetView* p_View);
    void ChooseArmedTarget();
    void FlipAtPresent(IDXGISwapChain* p_SwapChain);
    void FinishFrame(IDXGISwapChain* p_SwapChain, bool p_FlippedBeforeUi, bool p_FlippedAtPresent);

    const Settings& m_Settings;
    SceneState& m_Scene;

    MirrorFlip m_Flip;
    std::atomic<bool> m_Ready = false;
    std::string m_Error;
    std::atomic<bool> m_EnginePathDisabled = false;
    std::atomic<uint64_t> m_PresentsSinceInstall = 0;
    std::atomic<ID3D12GraphicsCommandList*> m_FlippedList = nullptr;
    std::atomic<uint32_t> m_ScreenWidth = 0;
    std::atomic<uint32_t> m_ScreenHeight = 0;

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_PresentList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_PresentFence;
    HANDLE m_PresentEvent = nullptr;
    PresentFrame m_PresentFrames[c_FrameContexts];
    uint64_t m_PresentFrameIndex = 0;
    uint64_t m_NextFenceValue = 0;

    ID3D12Resource* m_ArmedTarget = nullptr;
    std::atomic<bool> m_FlippedThisFrame = false;

    // The current UI pass and the frame so far. Only draws into a screen sized target count as drawing the UI.
    bool m_PassDrawsToScreen = false;
    uint64_t m_DrawsAtPassStart = 0;
    uint32_t m_FrameUiPasses = 0;
    uint64_t m_FrameUiDraws = 0;

    std::atomic<int> m_FramesToTrace = 0;

    std::atomic<uint64_t> m_FlipsBeforeUi = 0;
    std::atomic<uint64_t> m_FlipsAtPresent = 0;
    std::atomic<uint64_t> m_FlipsAborted = 0;
    std::atomic<uint64_t> m_TargetsRejected = 0;
};
