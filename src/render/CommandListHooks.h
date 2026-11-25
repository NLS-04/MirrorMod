// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <directx/d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

/**
 * Vtable hooks on ID3D12GraphicsCommandList and on the swap chain.
 *
 * They find the command list the engine is recording while it draws the UI: the first draw call on a thread
 * inside an armed region arrives with that list as its `this` pointer.
 *
 * Inside a render pass copies and barriers would make ID3D12CommandList::Close fail. The D3D12 runtime routes
 * draws of a render pass past these hooks, so they never trigger the flip there (covered by MirrorFlipTest).
 *
 * They also remember the pipeline state and compute root signature per command list, because D3D12 has no
 * getters for those and the flip pass has to put them back.
 */
namespace CommandListHooks {
    struct Callbacks {
        void* m_Context = nullptr;

        /** The first spot to record the flip was reached, right before a draw call. */
        void (*m_OnArmedSite)(
            void* p_Context,
            ID3D12GraphicsCommandList* p_List,
            ID3D12PipelineState* p_Pipeline,
            ID3D12RootSignature* p_ComputeRootSignature
        ) = nullptr;

        /** Runs before each real present, on the thread that presents. Independent of the SDK overlay. */
        void (*m_OnPresent)(void* p_Context, IDXGISwapChain* p_SwapChain) = nullptr;

        /** A viewport is being set. Only called while viewport tracing is on. */
        void (*m_OnViewport)(void* p_Context, const D3D12_VIEWPORT& p_Viewport) = nullptr;

        /** A command list failed to close. The engine aborts on this, so it is the last chance to log it. */
        void (*m_OnCloseFailed)(void* p_Context, ID3D12GraphicsCommandList* p_List, HRESULT p_Result) = nullptr;
    };

    struct Stats {
        uint64_t m_PipelineUnknown = 0;
    };

    /**
     * Patches the vtables of the command lists of p_Device and of p_SwapChain's class.
     * p_SwapChain may be null. Safe to call more than once.
     */
    bool Install(ID3D12Device* p_Device, IDXGISwapChain* p_SwapChain, const Callbacks& p_Callbacks);
    void Uninstall();

    /** While armed on the calling thread, draw calls on that thread invoke m_OnArmedSite first. */
    void SetArmed(bool p_Armed);

    void SetViewportTracing(bool p_Enabled);

    /**
     * Resources the calling thread moved into the render target state this frame and has not moved out again,
     * newest first. Returns how many were written. The target a pass draws into is the newest of them.
     */
    int GetRecentRenderTargets(ID3D12Resource** p_Out, int p_Max);

    /** Starts a new frame for GetRecentRenderTargets. The present hook does this on its own. */
    void AdvanceFrame();

    /**
     * The direct queue that most recently executed command lists, or null if none has yet.
     * This is how the present queue is found: D3D12 swap chains do not hand it out, GetDevice fails.
     */
    ID3D12CommandQueue* GetLastDirectQueue();

    /** How many draw calls the calling thread has made so far. */
    uint64_t GetDrawCount();

    Stats GetStats();
}
