// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/FrameMirror.h"

#include "Settings.h"
#include "core/GameMemory.h"
#include "core/Log.h"
#include "diagnostics/UiTrace.h"
#include "game/SceneState.h"
#include "hud/HudMirror.h"
#include "render/CommandListHooks.h"

#include <Globals.h>

#include <algorithm>
#include <format>

using Microsoft::WRL::ComPtr;

namespace {
    constexpr const char* c_Log = "render";

    constexpr uint32_t c_MaxBackBuffers = 8;
    constexpr int c_MaxRecentTargets = 8;

    // Lists that were already recording when the hooks went in have an unknown state, so wait those out.
    constexpr uint64_t c_PresentsBeforeArming = 3;

    // The render target view's layout is not documented. It holds its texture at this offset, like the depth view.
    constexpr size_t c_ViewTexture = 0x28;
    constexpr size_t c_ViewFirstField = 0x8;
    constexpr size_t c_ViewLastField = 0x80;

    // ZRenderTexture2D.
    constexpr size_t c_TextureResource = 0x8;
    constexpr size_t c_TextureSize = 0x28;

    Log::Budget g_ResolveLines(20);
    Log::Budget g_FlipLines(8);
    Log::Budget g_AbortLines(8);
    Log::Budget g_PresentFailureLines(8);
    Log::Budget g_MirroredUiLines(8);

    bool LooksLikePointer(uintptr_t p_Value) {
        return p_Value >= 0x10000 && p_Value < 0x7FFFFFFFFFFF && (p_Value & 7) == 0;
    }

    uintptr_t ReadPointer(uintptr_t p_Address) {
        uintptr_t s_Value = 0;
        return GameMemory::TryCopy(reinterpret_cast<const void*>(p_Address), &s_Value, sizeof(s_Value)) ? s_Value : 0;
    }

    bool IsD3DResource(void* p_Candidate) {
        __try {
            ID3D12Resource* s_Resource = nullptr;

            if (FAILED(static_cast<IUnknown*>(p_Candidate)->QueryInterface(IID_PPV_ARGS(&s_Resource)))) {
                return false;
            }

            s_Resource->Release();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string Describe(ID3D12Resource* p_Resource) {
        const D3D12_RESOURCE_DESC s_Desc = p_Resource->GetDesc();

        return std::format(
            "{} {}x{} format {} flags {:#x}", static_cast<void*>(p_Resource), s_Desc.Width, s_Desc.Height, static_cast<int>(s_Desc.Format),
            static_cast<int>(s_Desc.Flags)
        );
    }

    /** Whether the view renders to the swap chain. Tries every field, so it does not assume where the texture is. */
    bool LeadsToBackBuffer(uintptr_t p_View, ID3D12Resource* const* p_BackBuffers, UINT p_Count) {
        for (size_t s_Offset = c_ViewFirstField; s_Offset <= c_ViewLastField; s_Offset += sizeof(void*)) {
            const uintptr_t s_Texture = ReadPointer(p_View + s_Offset);

            if (!LooksLikePointer(s_Texture)) {
                continue;
            }

            const uintptr_t s_Resource = ReadPointer(s_Texture + c_TextureResource);

            for (UINT i = 0; s_Resource != 0 && i < p_Count; ++i) {
                if (s_Resource == reinterpret_cast<uintptr_t>(p_BackBuffers[i])) {
                    return true;
                }
            }
        }

        return false;
    }

    ID3D12Resource* FindIntermediateTarget(uintptr_t p_View, uint32_t p_Width, uint32_t p_Height) {
        const uintptr_t s_Texture = ReadPointer(p_View + c_ViewTexture);

        if (!LooksLikePointer(s_Texture)) {
            return nullptr;
        }

        uint32_t s_Size[2] {};
        const uintptr_t s_Resource = ReadPointer(s_Texture + c_TextureResource);

        if (!GameMemory::TryCopy(reinterpret_cast<const void*>(s_Texture + c_TextureSize), s_Size, sizeof(s_Size)) ||
            !LooksLikePointer(s_Resource) || s_Size[0] != p_Width || s_Size[1] != p_Height ||
            !IsD3DResource(reinterpret_cast<void*>(s_Resource))) {
            return nullptr;
        }

        return reinterpret_cast<ID3D12Resource*>(s_Resource);
    }
}

FrameMirror::FrameMirror(const Settings& p_Settings, SceneState& p_Scene) : m_Settings(p_Settings), m_Scene(p_Scene) {
}

FrameMirror::~FrameMirror() {
    if (m_PresentEvent) {
        CloseHandle(m_PresentEvent);
    }
}

void FrameMirror::Shutdown() {
    CommandListHooks::Uninstall();
}

void FrameMirror::TraceFrames(int p_Frames) {
    m_FramesToTrace = p_Frames;
}

FrameMirror::Stats FrameMirror::GetStats() const {
    Stats s_Stats;
    s_Stats.m_FlipsBeforeUi = m_FlipsBeforeUi.load(std::memory_order_relaxed);
    s_Stats.m_FlipsAtPresent = m_FlipsAtPresent.load(std::memory_order_relaxed);
    s_Stats.m_FlipsAborted = m_FlipsAborted.load(std::memory_order_relaxed);
    s_Stats.m_TargetsRejected = m_TargetsRejected.load(std::memory_order_relaxed);
    return s_Stats;
}

bool FrameMirror::BeforeUiPass(ZRenderTargetView** p_Views, bool p_CaptureOnly) {
    m_PassDrawsToScreen = false;

    // This is also what brings the hooks up, including the present hook the whole frame mode needs.
    if (!m_Settings.m_Enabled.Get() || !EnsureReady() || m_Settings.GetFlipMode() != FlipMode::BeforeUi) {
        return false;
    }

    ++m_FrameUiPasses;
    m_DrawsAtPassStart = CommandListHooks::GetDrawCount();

    // The number of passes changes from frame to frame: capture-only passes and passes without a screen target can come
    // first. So the decision is made per pass, and the first one drawing to the screen gets the flip.
    if (p_CaptureOnly || !p_Views || m_FlippedThisFrame) {
        return false;
    }

    ID3D12Resource* s_Target = ResolveFlipTarget(*p_Views);

    if (!s_Target) {
        m_TargetsRejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    m_PassDrawsToScreen = true;

    if (m_EnginePathDisabled || m_PresentsSinceInstall < c_PresentsBeforeArming || !m_Scene.IsGameVisible()) {
        return false;
    }

    m_ArmedTarget = s_Target;
    CommandListHooks::SetArmed(true);
    return true;
}

void FrameMirror::AfterUiPass(bool p_Armed) {
    if (p_Armed) {
        CommandListHooks::SetArmed(false);
        m_ArmedTarget = nullptr;
    }

    if (m_PassDrawsToScreen) {
        m_FrameUiDraws += CommandListHooks::GetDrawCount() - m_DrawsAtPassStart;
    }
}

bool FrameMirror::EnsureReady() {
    if (m_Ready) {
        return true;
    }

    if (!m_Error.empty() || !Globals::RenderManager || !Globals::RenderManager->m_pDevice) {
        return false;
    }

    auto* s_RenderDevice = Globals::RenderManager->m_pDevice;
    auto* s_Device = s_RenderDevice->m_pDevice;
    auto* s_SwapChain = s_RenderDevice->m_pSwapChain ? s_RenderDevice->m_pSwapChain->m_pSwapChain : nullptr;

    if (!s_Device || !s_SwapChain) {
        return false;
    }

    const auto s_Fail = [&](const std::string& p_Error) {
        m_Error = p_Error;
        Log::Error(c_Log, "Renderer setup failed: {}", p_Error);
        return false;
    };

    std::string s_Error;

    if (!m_Flip.Initialize(s_Device, &s_Error) || !CreatePresentResources(s_Device, &s_Error)) {
        return s_Fail(s_Error);
    }

    CommandListHooks::Callbacks s_Callbacks;
    s_Callbacks.m_Context = this;
    s_Callbacks.m_OnArmedSite = &FrameMirror::OnArmedSite;
    s_Callbacks.m_OnPresent = &FrameMirror::OnPresent;
    s_Callbacks.m_OnViewport = &FrameMirror::OnViewport;
    s_Callbacks.m_OnCloseFailed = &FrameMirror::OnCloseFailed;

    if (!CommandListHooks::Install(s_Device, s_SwapChain, s_Callbacks)) {
        return s_Fail("Could not hook the command list vtable.");
    }

    Log::Info(c_Log, "Renderer ready.");
    m_Ready = true;
    return true;
}

bool FrameMirror::CreatePresentResources(ID3D12Device* p_Device, std::string* p_Error) {
    const auto s_Fail = [&](const char* p_Step, HRESULT p_Result) {
        *p_Error = std::format("{} failed with {:#x}", p_Step, static_cast<uint32_t>(p_Result));
        return false;
    };

    for (auto& s_Frame : m_PresentFrames) {
        if (const HRESULT s_Result = p_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_Frame.m_Allocator)); FAILED(s_Result)) {
            return s_Fail("CreateCommandAllocator", s_Result);
        }
    }

    if (const HRESULT s_Result = p_Device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_PresentFrames[0].m_Allocator.Get(), nullptr, IID_PPV_ARGS(&m_PresentList)
    ); FAILED(s_Result)) {
        return s_Fail("CreateCommandList", s_Result);
    }

    m_PresentList->Close();

    if (const HRESULT s_Result = p_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_PresentFence)); FAILED(s_Result)) {
        return s_Fail("CreateFence", s_Result);
    }

    m_PresentEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!m_PresentEvent) {
        return s_Fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    }

    return true;
}

ID3D12Resource* FrameMirror::ResolveFlipTarget(ZRenderTargetView* p_View) {
    auto* s_RenderDevice = Globals::RenderManager->m_pDevice;

    if (!p_View || !s_RenderDevice->m_pSwapChain || !s_RenderDevice->m_pSwapChain->m_pSwapChain) {
        return nullptr;
    }

    IDXGISwapChain* s_SwapChain = s_RenderDevice->m_pSwapChain->m_pSwapChain;
    DXGI_SWAP_CHAIN_DESC s_SwapDesc {};

    if (FAILED(s_SwapChain->GetDesc(&s_SwapDesc))) {
        return nullptr;
    }

    const uint32_t s_Width = s_SwapDesc.BufferDesc.Width;
    const uint32_t s_Height = s_SwapDesc.BufferDesc.Height;

    m_ScreenWidth = s_Width;
    m_ScreenHeight = s_Height;
    HudMirror::SetScreenWidth(s_Width);

    // Raw pointers used only for comparing. Holding references would block swap chain resizes.
    ID3D12Resource* s_BackBuffers[c_MaxBackBuffers] {};
    const UINT s_BufferCount = (std::min)(s_SwapDesc.BufferCount, c_MaxBackBuffers);

    for (UINT i = 0; i < s_BufferCount; ++i) {
        ComPtr<ID3D12Resource> s_Buffer;

        if (SUCCEEDED(s_SwapChain->GetBuffer(i, IID_PPV_ARGS(&s_Buffer)))) {
            s_BackBuffers[i] = s_Buffer.Get();
        }
    }

    const auto s_ViewAddress = reinterpret_cast<uintptr_t>(p_View);
    const bool s_IsBackBuffer = LeadsToBackBuffer(s_ViewAddress, s_BackBuffers, s_BufferCount);
    ID3D12Resource* s_Target = nullptr;

    if (s_IsBackBuffer) {
        // The view holds one entry per back buffer, so a match only proves that it renders to the swap chain.
        // The buffer being drawn this frame is always the current one.
        ComPtr<IDXGISwapChain3> s_SwapChain3;

        if (FAILED(s_SwapChain->QueryInterface(IID_PPV_ARGS(&s_SwapChain3)))) {
            return nullptr;
        }

        const UINT s_Current = s_SwapChain3->GetCurrentBackBufferIndex();
        s_Target = s_Current < s_BufferCount ? s_BackBuffers[s_Current] : nullptr;
    }
    else {
        s_Target = FindIntermediateTarget(s_ViewAddress, s_Width, s_Height);
    }

    if (Log::IsVerbose() && g_ResolveLines.Take()) {
        Log::Debug(
            c_Log, "UI pass view {} -> target {} (back buffer: {}, screen {}x{}).", static_cast<void*>(p_View), static_cast<void*>(s_Target),
            s_IsBackBuffer, s_Width, s_Height
        );
    }

    return s_Target;
}

void FrameMirror::ChooseArmedTarget() {
    // The view handed to the UI pass names the back buffer, but the frame may still be on its way there through an
    // intermediate target. The target that really receives the UI is the newest one prepared for rendering.
    const D3D12_RESOURCE_DESC s_ScreenDesc = m_ArmedTarget->GetDesc();
    ID3D12Resource* s_Recent[c_MaxRecentTargets] {};
    const int s_RecentCount = CommandListHooks::GetRecentRenderTargets(s_Recent, c_MaxRecentTargets);

    for (int i = 0; i < s_RecentCount; ++i) {
        const D3D12_RESOURCE_DESC s_Desc = s_Recent[i]->GetDesc();

        if (s_Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && s_Desc.Width == s_ScreenDesc.Width && s_Desc.Height == s_ScreenDesc.Height &&
            s_Desc.SampleDesc.Count == 1 && (s_Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            m_ArmedTarget = s_Recent[i];
            return;
        }
    }
}

void FrameMirror::OnArmedSite(
    void* p_Context, ID3D12GraphicsCommandList* p_List, ID3D12PipelineState* p_Pipeline, ID3D12RootSignature* p_ComputeRootSignature
) {
    auto* s_This = static_cast<FrameMirror*>(p_Context);

    if (s_This->m_FlippedThisFrame || !s_This->m_ArmedTarget || s_This->m_EnginePathDisabled) {
        return;
    }

    s_This->ChooseArmedTarget();

    // A command list that fails to close makes the engine abort. Rehearse before touching its list.
    std::string s_Error;

    if (!s_This->m_Flip.Validate(s_This->m_ArmedTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, &s_Error)) {
        s_This->m_FlipsAborted.fetch_add(1, std::memory_order_relaxed);
        s_This->m_EnginePathDisabled = true;
        Log::Error(c_Log, "Flip rehearsal failed, falling back to flipping whole frames: {}", s_Error);
        return;
    }

    if (Log::IsVerbose() && g_FlipLines.Take()) {
        Log::Debug(
            c_Log, "Flip before UI: list {}, pipeline {}, target {}, thread {}.", static_cast<void*>(p_List), static_cast<void*>(p_Pipeline),
            Describe(s_This->m_ArmedTarget), GetCurrentThreadId()
        );
    }

    if (!s_This->m_Flip.Record(p_List, s_This->m_ArmedTarget, D3D12_RESOURCE_STATE_RENDER_TARGET)) {
        s_This->m_FlipsAborted.fetch_add(1, std::memory_order_relaxed);

        if (g_AbortLines.Take()) {
            Log::Warning(c_Log, "Could not record the flip for target {}.", Describe(s_This->m_ArmedTarget));
        }

        return;
    }

    if (p_Pipeline) {
        p_List->SetPipelineState(p_Pipeline);
    }

    if (p_ComputeRootSignature) {
        p_List->SetComputeRootSignature(p_ComputeRootSignature);
    }

    s_This->m_FlippedList = p_List;
    s_This->m_FlippedThisFrame = true;
    s_This->m_FlipsBeforeUi.fetch_add(1, std::memory_order_relaxed);
}

void FrameMirror::OnPresent(void* p_Context, IDXGISwapChain* p_SwapChain) {
    auto* s_This = static_cast<FrameMirror*>(p_Context);

    s_This->m_PresentsSinceInstall.fetch_add(1, std::memory_order_relaxed);

    // A frame without a UI draw never reached an armed site, and has nothing to keep unmirrored.
    const bool s_FlippedBeforeUi = s_This->m_FlippedThisFrame.exchange(false);
    const bool s_FlipNow = s_This->m_Settings.m_Enabled.Get() && !s_FlippedBeforeUi && s_This->m_Ready && s_This->m_Scene.IsGameVisible();

    if (s_FlipNow) {
        s_This->FlipAtPresent(p_SwapChain);
    }

    s_This->FinishFrame(p_SwapChain, s_FlippedBeforeUi, s_FlipNow);
}

void FrameMirror::FlipAtPresent(IDXGISwapChain* p_PresentSwapChain) {
    const auto s_Fail = [this](const char* p_Step, HRESULT p_Result) {
        m_FlipsAborted.fetch_add(1, std::memory_order_relaxed);

        if (g_PresentFailureLines.Take()) {
            Log::Warning(c_Log, "Flip at present failed: {} returned {:#x}.", p_Step, static_cast<uint32_t>(p_Result));
        }
    };

    ComPtr<IDXGISwapChain3> s_SwapChain;

    if (const HRESULT s_Result = p_PresentSwapChain->QueryInterface(IID_PPV_ARGS(&s_SwapChain)); FAILED(s_Result)) {
        return s_Fail("QueryInterface(IDXGISwapChain3)", s_Result);
    }

    // The flip has to run on the queue that presents, or it lands after the frame was already shown.
    ID3D12CommandQueue* s_Queue = CommandListHooks::GetLastDirectQueue();

    // Nothing was submitted yet since the hooks went in, so there is no frame to flip.
    if (!s_Queue) {
        return;
    }

    ComPtr<ID3D12Resource> s_BackBuffer;

    if (const HRESULT s_Result = s_SwapChain->GetBuffer(s_SwapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&s_BackBuffer)); FAILED(s_Result)) {
        return s_Fail("GetBuffer", s_Result);
    }

    auto& s_Frame = m_PresentFrames[++m_PresentFrameIndex % c_FrameContexts];

    if (s_Frame.m_FenceValue != 0 && m_PresentFence->GetCompletedValue() < s_Frame.m_FenceValue) {
        m_PresentFence->SetEventOnCompletion(s_Frame.m_FenceValue, m_PresentEvent);
        WaitForSingleObject(m_PresentEvent, INFINITE);
    }

    s_Frame.m_Allocator->Reset();
    m_PresentList->Reset(s_Frame.m_Allocator.Get(), nullptr);

    const bool s_Recorded = m_Flip.Record(m_PresentList.Get(), s_BackBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT);
    const HRESULT s_Closed = m_PresentList->Close();

    if (!s_Recorded || FAILED(s_Closed)) {
        return s_Fail(s_Recorded ? "Close" : "Record", s_Closed);
    }

    ID3D12CommandList* s_Lists[] = { m_PresentList.Get() };
    s_Queue->ExecuteCommandLists(1, s_Lists);

    s_Frame.m_FenceValue = ++m_NextFenceValue;
    s_Queue->Signal(m_PresentFence.Get(), s_Frame.m_FenceValue);

    m_FlipsAtPresent.fetch_add(1, std::memory_order_relaxed);
}

void FrameMirror::FinishFrame(IDXGISwapChain* p_SwapChain, bool p_FlippedBeforeUi, bool p_FlippedAtPresent) {
    // Flipping a whole frame mirrors the UI it contains, which should only happen on the fallback path.
    if (p_FlippedAtPresent && m_FrameUiDraws > 0 && !m_EnginePathDisabled && m_PresentsSinceInstall > c_PresentsBeforeArming && g_MirroredUiLines.Take()) {
        Log::Warning(c_Log, "A frame with {} UI draws missed the flip before the UI, so its UI got mirrored as well.", m_FrameUiDraws);
    }

    if (m_FramesToTrace.load(std::memory_order_relaxed) > 0) {
        m_FramesToTrace.fetch_sub(1, std::memory_order_relaxed);

        ComPtr<IDXGISwapChain3> s_SwapChain;
        const int s_BackBuffer = SUCCEEDED(p_SwapChain->QueryInterface(IID_PPV_ARGS(&s_SwapChain))) ? static_cast<int>(s_SwapChain->GetCurrentBackBufferIndex()) : -1;

        Log::Info(
            "frame", "backBuffer={} uiPasses={} uiDraws={} flip={}", s_BackBuffer, m_FrameUiPasses, m_FrameUiDraws,
            p_FlippedBeforeUi ? "before-ui" : p_FlippedAtPresent ? "at-present" : "none"
        );
    }

    m_FrameUiPasses = 0;
    m_FrameUiDraws = 0;
}

void FrameMirror::OnViewport(void* p_Context, const D3D12_VIEWPORT& p_Viewport) {
    auto* s_This = static_cast<FrameMirror*>(p_Context);

    const uint32_t s_Width = s_This->m_ScreenWidth;
    const uint32_t s_Height = s_This->m_ScreenHeight;

    if (s_Width == 0 || (p_Viewport.Width >= s_Width - 1 && p_Viewport.Height >= s_Height - 1)) {
        return;
    }

    ID3D12Resource* s_Recent[1] {};

    if (CommandListHooks::GetRecentRenderTargets(s_Recent, 1) == 0) {
        return;
    }

    // Keeps the shadow maps and other small targets out, only a rectangle on a full size target can be the PiP.
    const D3D12_RESOURCE_DESC s_Desc = s_Recent[0]->GetDesc();

    if (s_Desc.Width == s_Width && s_Desc.Height == s_Height) {
        UiTrace::NoteViewport(p_Viewport.TopLeftX, p_Viewport.TopLeftY, p_Viewport.Width, p_Viewport.Height, s_Recent[0], s_This->m_FlippedThisFrame);
    }
}

void FrameMirror::OnCloseFailed(void* p_Context, ID3D12GraphicsCommandList* p_List, HRESULT p_Result) {
    auto* s_This = static_cast<FrameMirror*>(p_Context);

    Log::Error(
        c_Log, "Closing command list {} failed with {:#x}. Last list with a flip: {}.", static_cast<void*>(p_List), static_cast<uint32_t>(p_Result),
        static_cast<void*>(s_This->m_FlippedList.load())
    );
}
