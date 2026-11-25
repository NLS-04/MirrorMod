// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/CommandListHooks.h"

#include "core/Log.h"
#include "core/VtableHook.h"

#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace {
    // Vtable slots, fixed by the COM ABI.
    constexpr size_t c_SlotClose = 9;
    constexpr size_t c_SlotReset = 10;
    constexpr size_t c_SlotDrawInstanced = 12;
    constexpr size_t c_SlotDrawIndexedInstanced = 13;
    constexpr size_t c_SlotRSSetViewports = 21;
    constexpr size_t c_SlotSetPipelineState = 25;
    constexpr size_t c_SlotResourceBarrier = 26;
    constexpr size_t c_SlotSetComputeRootSignature = 29;

    constexpr size_t c_SlotExecuteCommandLists = 10;

    constexpr size_t c_SlotPresent = 8;
    constexpr size_t c_SlotPresent1 = 22;

    using List = ID3D12GraphicsCommandList;

    using Close_t = HRESULT(STDMETHODCALLTYPE*)(List*);
    using Reset_t = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    using DrawInstanced_t = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT);
    using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
    using RSSetViewports_t = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);
    using SetPipelineState_t = void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
    using ResourceBarrier_t = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RESOURCE_BARRIER*);
    using SetComputeRootSignature_t = void(STDMETHODCALLTYPE*)(List*, ID3D12RootSignature*);
    using ExecuteCommandLists_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1_t = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*
    );

    Close_t g_Close = nullptr;
    Reset_t g_Reset = nullptr;
    DrawInstanced_t g_DrawInstanced = nullptr;
    DrawIndexedInstanced_t g_DrawIndexedInstanced = nullptr;
    RSSetViewports_t g_RSSetViewports = nullptr;
    SetPipelineState_t g_SetPipelineState = nullptr;
    ResourceBarrier_t g_ResourceBarrier = nullptr;
    SetComputeRootSignature_t g_SetComputeRootSignature = nullptr;
    ExecuteCommandLists_t g_ExecuteCommandLists = nullptr;
    Present_t g_Present = nullptr;
    Present1_t g_Present1 = nullptr;

    void** g_ListVtable = nullptr;
    void** g_QueueVtable = nullptr;
    void** g_SwapChainVtable = nullptr;
    CommandListHooks::Callbacks g_Callbacks;

    std::atomic<ID3D12CommandQueue*> g_LastDirectQueue = nullptr;
    std::atomic<uint64_t> g_Frame = 0;
    std::atomic<bool> g_ViewportTracing = false;

    std::atomic<uint64_t> g_PipelineUnknown = 0;

    Log::Budget g_PipelineLines(4);

    constexpr int c_MaxRecentTargets = 16;

    struct ListState {
        ID3D12PipelineState* m_Pipeline = nullptr;
        ID3D12RootSignature* m_ComputeRootSignature = nullptr;
    };

    struct ThreadState {
        List* m_CachedList = nullptr;
        ListState* m_CachedState = nullptr;
        std::unordered_map<List*, ListState> m_Lists;
        bool m_Armed = false;
        uint64_t m_Draws = 0;

        // Resources currently in the render target state, oldest first, for the current frame.
        ID3D12Resource* m_Recent[c_MaxRecentTargets] {};
        int m_RecentCount = 0;
        uint64_t m_RecentFrame = 0;
    };

    thread_local ThreadState t_State;

    ListState& StateFor(List* p_List) {
        if (t_State.m_CachedList != p_List) {
            t_State.m_CachedState = &t_State.m_Lists[p_List];
            t_State.m_CachedList = p_List;
        }
        return *t_State.m_CachedState;
    }

    void BeforeDraw(List* p_List) {
        ++t_State.m_Draws;

        if (!t_State.m_Armed) {
            return;
        }

        if (!g_Callbacks.m_OnArmedSite || p_List->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return;
        }

        const ListState& s_State = StateFor(p_List);

        // The pipeline must be restorable, because this very draw runs right after the flip.
        if (!s_State.m_Pipeline) {
            g_PipelineUnknown.fetch_add(1, std::memory_order_relaxed);

            if (g_PipelineLines.Take()) {
                Log::Warning("render", "A draw call was skipped as the place for the flip, because its pipeline state is unknown.");
            }

            return;
        }

        g_Callbacks.m_OnArmedSite(g_Callbacks.m_Context, p_List, s_State.m_Pipeline, s_State.m_ComputeRootSignature);
    }

    HRESULT STDMETHODCALLTYPE CloseDetour(List* p_List) {
        t_State.m_Lists.erase(p_List);
        t_State.m_CachedList = nullptr;
        t_State.m_CachedState = nullptr;

        const HRESULT s_Result = g_Close(p_List);

        if (FAILED(s_Result) && g_Callbacks.m_OnCloseFailed) {
            g_Callbacks.m_OnCloseFailed(g_Callbacks.m_Context, p_List, s_Result);
        }

        return s_Result;
    }

    HRESULT STDMETHODCALLTYPE ResetDetour(List* p_List, ID3D12CommandAllocator* p_Allocator, ID3D12PipelineState* p_Initial) {
        ListState& s_State = StateFor(p_List);
        s_State = {};
        s_State.m_Pipeline = p_Initial;
        return g_Reset(p_List, p_Allocator, p_Initial);
    }

    void STDMETHODCALLTYPE DrawInstancedDetour(List* p_List, UINT p_Vertices, UINT p_Instances, UINT p_Start, UINT p_StartInstance) {
        BeforeDraw(p_List);
        g_DrawInstanced(p_List, p_Vertices, p_Instances, p_Start, p_StartInstance);
    }

    void STDMETHODCALLTYPE DrawIndexedInstancedDetour(
        List* p_List, UINT p_Indices, UINT p_Instances, UINT p_Start, INT p_BaseVertex, UINT p_StartInstance
    ) {
        BeforeDraw(p_List);
        g_DrawIndexedInstanced(p_List, p_Indices, p_Instances, p_Start, p_BaseVertex, p_StartInstance);
    }

    void STDMETHODCALLTYPE RSSetViewportsDetour(List* p_List, UINT p_Count, const D3D12_VIEWPORT* p_Viewports) {
        if (p_Count > 0 && g_ViewportTracing.load(std::memory_order_relaxed) && g_Callbacks.m_OnViewport) {
            g_Callbacks.m_OnViewport(g_Callbacks.m_Context, p_Viewports[0]);
        }

        g_RSSetViewports(p_List, p_Count, p_Viewports);
    }

    void STDMETHODCALLTYPE SetPipelineStateDetour(List* p_List, ID3D12PipelineState* p_Pipeline) {
        ListState& s_State = StateFor(p_List);
        s_State.m_Pipeline = p_Pipeline;
        g_SetPipelineState(p_List, p_Pipeline);
    }

    void ForgetRecentTarget(ID3D12Resource* p_Resource) {
        for (int i = 0; i < t_State.m_RecentCount; ++i) {
            if (t_State.m_Recent[i] == p_Resource) {
                for (int j = i + 1; j < t_State.m_RecentCount; ++j) {
                    t_State.m_Recent[j - 1] = t_State.m_Recent[j];
                }
                --t_State.m_RecentCount;
                return;
            }
        }
    }

    void STDMETHODCALLTYPE ResourceBarrierDetour(List* p_List, UINT p_Count, const D3D12_RESOURCE_BARRIER* p_Barriers) {
        const uint64_t s_Frame = g_Frame.load(std::memory_order_relaxed);

        if (t_State.m_RecentFrame != s_Frame) {
            t_State.m_RecentFrame = s_Frame;
            t_State.m_RecentCount = 0;
        }

        for (UINT i = 0; i < p_Count; ++i) {
            if (p_Barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }

            const auto& s_Transition = p_Barriers[i].Transition;

            if (s_Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET ||
                s_Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                ForgetRecentTarget(s_Transition.pResource);
            }

            if (s_Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                if (t_State.m_RecentCount == c_MaxRecentTargets) {
                    ForgetRecentTarget(t_State.m_Recent[0]);
                }
                t_State.m_Recent[t_State.m_RecentCount++] = s_Transition.pResource;
            }
        }

        g_ResourceBarrier(p_List, p_Count, p_Barriers);
    }

    void STDMETHODCALLTYPE SetComputeRootSignatureDetour(List* p_List, ID3D12RootSignature* p_Signature) {
        StateFor(p_List).m_ComputeRootSignature = p_Signature;
        g_SetComputeRootSignature(p_List, p_Signature);
    }

    void STDMETHODCALLTYPE ExecuteCommandListsDetour(
        ID3D12CommandQueue* p_Queue, UINT p_Count, ID3D12CommandList* const* p_Lists
    ) {
        if (p_Count > 0 && p_Queue != g_LastDirectQueue.load(std::memory_order_relaxed) &&
            p_Queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_LastDirectQueue.store(p_Queue, std::memory_order_relaxed);
        }

        g_ExecuteCommandLists(p_Queue, p_Count, p_Lists);
    }

    void NotifyPresent(IDXGISwapChain* p_SwapChain, UINT p_Flags) {
        // DXGI_PRESENT_TEST only probes the window state and shows nothing.
        if (p_Flags & DXGI_PRESENT_TEST) {
            return;
        }

        if (g_Callbacks.m_OnPresent) {
            g_Callbacks.m_OnPresent(g_Callbacks.m_Context, p_SwapChain);
        }

        CommandListHooks::AdvanceFrame();
    }

    HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* p_SwapChain, UINT p_Sync, UINT p_Flags) {
        NotifyPresent(p_SwapChain, p_Flags);
        return g_Present(p_SwapChain, p_Sync, p_Flags);
    }

    HRESULT STDMETHODCALLTYPE Present1Detour(
        IDXGISwapChain* p_SwapChain, UINT p_Sync, UINT p_Flags, const DXGI_PRESENT_PARAMETERS* p_Parameters
    ) {
        NotifyPresent(p_SwapChain, p_Flags);
        return g_Present1(p_SwapChain, p_Sync, p_Flags, p_Parameters);
    }

    template <class T>
    VtableHook::Slot MakeSlot(size_t p_Index, T p_Detour, T* p_Original) {
        return { p_Index, reinterpret_cast<void*>(p_Detour), reinterpret_cast<void**>(p_Original) };
    }

    const VtableHook::Slot c_ListSlots[] = {
        MakeSlot<Close_t>(c_SlotClose, CloseDetour, &g_Close),
        MakeSlot<Reset_t>(c_SlotReset, ResetDetour, &g_Reset),
        MakeSlot<DrawInstanced_t>(c_SlotDrawInstanced, DrawInstancedDetour, &g_DrawInstanced),
        MakeSlot<DrawIndexedInstanced_t>(c_SlotDrawIndexedInstanced, DrawIndexedInstancedDetour, &g_DrawIndexedInstanced),
        MakeSlot<RSSetViewports_t>(c_SlotRSSetViewports, RSSetViewportsDetour, &g_RSSetViewports),
        MakeSlot<SetPipelineState_t>(c_SlotSetPipelineState, SetPipelineStateDetour, &g_SetPipelineState),
        MakeSlot<ResourceBarrier_t>(c_SlotResourceBarrier, ResourceBarrierDetour, &g_ResourceBarrier),
        MakeSlot<SetComputeRootSignature_t>(c_SlotSetComputeRootSignature, SetComputeRootSignatureDetour, &g_SetComputeRootSignature),
    };

    const VtableHook::Slot c_QueueSlots[] = {
        MakeSlot<ExecuteCommandLists_t>(c_SlotExecuteCommandLists, ExecuteCommandListsDetour, &g_ExecuteCommandLists),
    };

    const VtableHook::Slot c_SwapChainSlots[] = {
        MakeSlot<Present_t>(c_SlotPresent, PresentDetour, &g_Present),
        MakeSlot<Present1_t>(c_SlotPresent1, Present1Detour, &g_Present1),
    };
}

bool CommandListHooks::Install(ID3D12Device* p_Device, IDXGISwapChain* p_SwapChain, const Callbacks& p_Callbacks) {
    if (g_ListVtable) {
        return true;
    }

    // All direct command lists from one device share a vtable, so a throwaway list exposes it.
    ComPtr<ID3D12CommandAllocator> s_Allocator;
    ComPtr<ID3D12GraphicsCommandList> s_List;

    if (FAILED(p_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_Allocator))) ||
        FAILED(p_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_Allocator.Get(), nullptr, IID_PPV_ARGS(&s_List)))) {
        Log::Error("render", "Could not create the command list used to find the vtable.");
        return false;
    }

    s_List->Close();

    g_Callbacks = p_Callbacks;

    void** s_ListVtable = *reinterpret_cast<void***>(s_List.Get());

    if (!VtableHook::Install(s_ListVtable, c_ListSlots)) {
        g_Callbacks = {};
        return false;
    }

    g_ListVtable = s_ListVtable;

    D3D12_COMMAND_QUEUE_DESC s_QueueDesc {};
    s_QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ComPtr<ID3D12CommandQueue> s_Queue;

    if (SUCCEEDED(p_Device->CreateCommandQueue(&s_QueueDesc, IID_PPV_ARGS(&s_Queue)))) {
        void** s_QueueVtable = *reinterpret_cast<void***>(s_Queue.Get());
        g_QueueVtable = VtableHook::Install(s_QueueVtable, c_QueueSlots) ? s_QueueVtable : nullptr;
    }

    if (!g_QueueVtable) {
        Log::Warning("render", "Could not hook the command queue, so whole frames cannot be flipped.");
    }

    if (p_SwapChain) {
        void** s_SwapChainVtable = *reinterpret_cast<void***>(p_SwapChain);
        g_SwapChainVtable = VtableHook::Install(s_SwapChainVtable, c_SwapChainSlots) ? s_SwapChainVtable : nullptr;

        if (!g_SwapChainVtable) {
            Log::Warning("render", "Could not hook the swap chain, so whole frames cannot be flipped.");
        }
    }

    return true;
}

void CommandListHooks::Uninstall() {
    if (!g_ListVtable) {
        return;
    }

    if (g_QueueVtable) {
        VtableHook::Remove(g_QueueVtable, c_QueueSlots);
        g_QueueVtable = nullptr;
    }

    g_LastDirectQueue = nullptr;

    if (g_SwapChainVtable) {
        VtableHook::Remove(g_SwapChainVtable, c_SwapChainSlots);
        g_SwapChainVtable = nullptr;
    }

    VtableHook::Remove(g_ListVtable, c_ListSlots);

    g_ListVtable = nullptr;
    g_Callbacks = {};
}

int CommandListHooks::GetRecentRenderTargets(ID3D12Resource** p_Out, int p_Max) {
    if (t_State.m_RecentFrame != g_Frame.load(std::memory_order_relaxed)) {
        return 0;
    }

    int s_Written = 0;

    for (int i = t_State.m_RecentCount - 1; i >= 0 && s_Written < p_Max; --i) {
        p_Out[s_Written++] = t_State.m_Recent[i];
    }

    return s_Written;
}

void CommandListHooks::SetViewportTracing(bool p_Enabled) {
    g_ViewportTracing = p_Enabled;
}

void CommandListHooks::AdvanceFrame() {
    g_Frame.fetch_add(1, std::memory_order_relaxed);
}

ID3D12CommandQueue* CommandListHooks::GetLastDirectQueue() {
    return g_LastDirectQueue.load(std::memory_order_relaxed);
}

void CommandListHooks::SetArmed(bool p_Armed) {
    t_State.m_Armed = p_Armed;
}

uint64_t CommandListHooks::GetDrawCount() {
    return t_State.m_Draws;
}

CommandListHooks::Stats CommandListHooks::GetStats() {
    Stats s_Stats;
    s_Stats.m_PipelineUnknown = g_PipelineUnknown.load(std::memory_order_relaxed);
    return s_Stats;
}
