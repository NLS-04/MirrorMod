// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone check of the flip pass and the command list hooks on the real GPU.
// Covers what can be verified without the game; see README for what still needs an in-game test.

#include "TestSupport.h"

#include "render/CommandListHooks.h"
#include "render/MirrorFlip.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <directx/d3d12sdklayers.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
    struct Gpu {
        ComPtr<ID3D12Device> m_Device;
        ComPtr<ID3D12CommandQueue> m_Queue;
        ComPtr<ID3D12CommandAllocator> m_Allocator;
        ComPtr<ID3D12GraphicsCommandList> m_List;
        ComPtr<ID3D12Fence> m_Fence;
        HANDLE m_Event = nullptr;
        uint64_t m_Value = 0;

        void Begin() {
            m_Allocator->Reset();
            m_List->Reset(m_Allocator.Get(), nullptr);
        }

        void SubmitAndWait() {
            m_List->Close();
            ID3D12CommandList* s_Lists[] = { m_List.Get() };
            m_Queue->ExecuteCommandLists(1, s_Lists);
            m_Queue->Signal(m_Fence.Get(), ++m_Value);
            m_Fence->SetEventOnCompletion(m_Value, m_Event);
            WaitForSingleObject(m_Event, INFINITE);
        }
    };

    ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* p_Device, D3D12_HEAP_TYPE p_Heap, uint64_t p_Size, D3D12_RESOURCE_STATES p_State) {
        D3D12_HEAP_PROPERTIES s_Heap {};
        s_Heap.Type = p_Heap;

        D3D12_RESOURCE_DESC s_Desc {};
        s_Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        s_Desc.Width = p_Size;
        s_Desc.Height = 1;
        s_Desc.DepthOrArraySize = 1;
        s_Desc.MipLevels = 1;
        s_Desc.SampleDesc.Count = 1;
        s_Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> s_Resource;
        p_Device->CreateCommittedResource(&s_Heap, D3D12_HEAP_FLAG_NONE, &s_Desc, p_State, nullptr, IID_PPV_ARGS(&s_Resource));
        return s_Resource;
    }

    struct Target {
        ComPtr<ID3D12Resource> m_Texture;
        ComPtr<ID3D12Resource> m_Upload;
        ComPtr<ID3D12Resource> m_Readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_Footprint {};
        UINT m_PixelSize = 0;
    };

    // Every byte encodes its position, so any wrong row, column or byte order is detected.
    uint8_t PatternByte(UINT p_X, UINT p_Y, UINT p_Byte) {
        return static_cast<uint8_t>((p_X * 7) ^ (p_Y * 13) ^ (p_Byte * 31) ^ (p_X >> 8) ^ (p_Y >> 5));
    }

    Target MakeTarget(Gpu& p_Gpu, UINT p_Width, UINT p_Height, DXGI_FORMAT p_Format, UINT p_PixelSize) {
        Target s_Target;
        s_Target.m_PixelSize = p_PixelSize;

        D3D12_HEAP_PROPERTIES s_Heap {};
        s_Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC s_Desc {};
        s_Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        s_Desc.Width = p_Width;
        s_Desc.Height = p_Height;
        s_Desc.DepthOrArraySize = 1;
        s_Desc.MipLevels = 1;
        s_Desc.Format = p_Format;
        s_Desc.SampleDesc.Count = 1;
        s_Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        p_Gpu.m_Device->CreateCommittedResource(
            &s_Heap, D3D12_HEAP_FLAG_NONE, &s_Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_Target.m_Texture)
        );

        UINT64 s_Total = 0;
        p_Gpu.m_Device->GetCopyableFootprints(&s_Desc, 0, 1, 0, &s_Target.m_Footprint, nullptr, nullptr, &s_Total);

        s_Target.m_Upload = MakeBuffer(p_Gpu.m_Device.Get(), D3D12_HEAP_TYPE_UPLOAD, s_Total, D3D12_RESOURCE_STATE_GENERIC_READ);
        s_Target.m_Readback = MakeBuffer(p_Gpu.m_Device.Get(), D3D12_HEAP_TYPE_READBACK, s_Total, D3D12_RESOURCE_STATE_COPY_DEST);

        uint8_t* s_Mapped = nullptr;
        s_Target.m_Upload->Map(0, nullptr, reinterpret_cast<void**>(&s_Mapped));

        for (UINT y = 0; y < p_Height; ++y) {
            for (UINT x = 0; x < p_Width; ++x) {
                for (UINT b = 0; b < p_PixelSize; ++b) {
                    s_Mapped[y * s_Target.m_Footprint.Footprint.RowPitch + x * p_PixelSize + b] = PatternByte(x, y, b);
                }
            }
        }

        s_Target.m_Upload->Unmap(0, nullptr);
        return s_Target;
    }

    D3D12_RESOURCE_BARRIER TextureBarrier(ID3D12Resource* p_Texture, D3D12_RESOURCE_STATES p_Before, D3D12_RESOURCE_STATES p_After) {
        D3D12_RESOURCE_BARRIER s_Barrier {};
        s_Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        s_Barrier.Transition.pResource = p_Texture;
        s_Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_Barrier.Transition.StateBefore = p_Before;
        s_Barrier.Transition.StateAfter = p_After;
        return s_Barrier;
    }

    void RecordUpload(Gpu& p_Gpu, const Target& p_Target) {
        D3D12_TEXTURE_COPY_LOCATION s_Src {};
        s_Src.pResource = p_Target.m_Upload.Get();
        s_Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s_Src.PlacedFootprint = p_Target.m_Footprint;

        D3D12_TEXTURE_COPY_LOCATION s_Dst {};
        s_Dst.pResource = p_Target.m_Texture.Get();
        s_Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        p_Gpu.m_List->CopyTextureRegion(&s_Dst, 0, 0, 0, &s_Src, nullptr);

        const auto s_Barrier = TextureBarrier(p_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);
    }

    void RecordReadback(Gpu& p_Gpu, const Target& p_Target) {
        const auto s_Barrier = TextureBarrier(p_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        p_Gpu.m_List->ResourceBarrier(1, &s_Barrier);

        D3D12_TEXTURE_COPY_LOCATION s_Src {};
        s_Src.pResource = p_Target.m_Texture.Get();
        s_Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION s_Dst {};
        s_Dst.pResource = p_Target.m_Readback.Get();
        s_Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s_Dst.PlacedFootprint = p_Target.m_Footprint;

        p_Gpu.m_List->CopyTextureRegion(&s_Dst, 0, 0, 0, &s_Src, nullptr);
    }

    bool IsMirrored(const Target& p_Target) {
        const auto& s_Footprint = p_Target.m_Footprint.Footprint;
        uint8_t* s_Mapped = nullptr;
        p_Target.m_Readback->Map(0, nullptr, reinterpret_cast<void**>(&s_Mapped));

        bool s_Ok = true;
        for (UINT y = 0; y < s_Footprint.Height && s_Ok; ++y) {
            for (UINT x = 0; x < s_Footprint.Width && s_Ok; ++x) {
                for (UINT b = 0; b < p_Target.m_PixelSize; ++b) {
                    const uint8_t s_Got = s_Mapped[y * s_Footprint.RowPitch + x * p_Target.m_PixelSize + b];
                    if (s_Got != PatternByte(s_Footprint.Width - 1 - x, y, b)) {
                        printf("  mismatch at x=%u y=%u byte=%u\n", x, y, b);
                        s_Ok = false;
                        break;
                    }
                }
            }
        }

        p_Target.m_Readback->Unmap(0, nullptr);
        return s_Ok;
    }

    void TestFlip(Gpu& p_Gpu, MirrorFlip& p_Flip, UINT p_Width, UINT p_Height, DXGI_FORMAT p_Format, UINT p_PixelSize, const char* p_Name) {
        Target s_Target = MakeTarget(p_Gpu, p_Width, p_Height, p_Format, p_PixelSize);

        p_Gpu.Begin();
        RecordUpload(p_Gpu, s_Target);
        const bool s_Recorded = p_Flip.Record(p_Gpu.m_List.Get(), s_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        RecordReadback(p_Gpu, s_Target);
        p_Gpu.SubmitAndWait();

        Check(s_Recorded && IsMirrored(s_Target), std::string("flip ") + p_Name);
    }

    ComPtr<ID3D12PipelineState> MakeDepthlessPipeline(
        ID3D12Device* p_Device, ComPtr<ID3D12RootSignature>& p_OutRoot, DXGI_FORMAT p_RenderTargetFormat = DXGI_FORMAT_UNKNOWN
    ) {
        D3D12_ROOT_SIGNATURE_DESC s_RootDesc {};
        ComPtr<ID3DBlob> s_Blob;
        D3D12SerializeRootSignature(&s_RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &s_Blob, nullptr);
        p_Device->CreateRootSignature(0, s_Blob->GetBufferPointer(), s_Blob->GetBufferSize(), IID_PPV_ARGS(&p_OutRoot));

        const char c_Vs[] = "float4 main() : SV_Position { return float4(2, 2, 0, 1); }";
        ComPtr<ID3DBlob> s_Vs;
        D3DCompile(c_Vs, sizeof(c_Vs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &s_Vs, nullptr);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC s_Desc {};
        s_Desc.pRootSignature = p_OutRoot.Get();
        s_Desc.VS = { s_Vs->GetBufferPointer(), s_Vs->GetBufferSize() };
        s_Desc.SampleMask = UINT_MAX;
        s_Desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        s_Desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        s_Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        s_Desc.SampleDesc.Count = 1;

        if (p_RenderTargetFormat != DXGI_FORMAT_UNKNOWN) {
            s_Desc.NumRenderTargets = 1;
            s_Desc.RTVFormats[0] = p_RenderTargetFormat;
        }

        ComPtr<ID3D12PipelineState> s_Pipeline;
        p_Device->CreateGraphicsPipelineState(&s_Desc, IID_PPV_ARGS(&s_Pipeline));
        return s_Pipeline;
    }

    struct CallbackState {
        MirrorFlip* m_Flip = nullptr;
        ID3D12Resource* m_Target = nullptr;
        int m_Calls = 0;
        ID3D12GraphicsCommandList* m_SeenList = nullptr;
        ID3D12PipelineState* m_SeenPipeline = nullptr;
        bool m_Recorded = false;
    };

    // Mirrors what MirrorMod::OnArmedDraw does.
    void OnArmedDraw(void* p_Context, ID3D12GraphicsCommandList* p_List, ID3D12PipelineState* p_Pipeline, ID3D12RootSignature* p_ComputeRoot) {
        auto* s_State = static_cast<CallbackState*>(p_Context);
        ++s_State->m_Calls;
        s_State->m_SeenList = p_List;
        s_State->m_SeenPipeline = p_Pipeline;

        s_State->m_Recorded = s_State->m_Flip->Record(p_List, s_State->m_Target, D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (p_Pipeline) {
            p_List->SetPipelineState(p_Pipeline);
        }
        if (p_ComputeRoot) {
            p_List->SetComputeRootSignature(p_ComputeRoot);
        }
    }

    void TestHooks(Gpu& p_Gpu, MirrorFlip& p_Flip) {
        Target s_Target = MakeTarget(p_Gpu, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 4);

        CallbackState s_State;
        s_State.m_Flip = &p_Flip;
        s_State.m_Target = s_Target.m_Texture.Get();

        CommandListHooks::Callbacks s_Callbacks;
        s_Callbacks.m_Context = &s_State;
        s_Callbacks.m_OnArmedSite = OnArmedDraw;
        Check(CommandListHooks::Install(p_Gpu.m_Device.Get(), nullptr, s_Callbacks), "hooks install");

        ComPtr<ID3D12RootSignature> s_Root;
        const auto s_Pipeline = MakeDepthlessPipeline(p_Gpu.m_Device.Get(), s_Root);

        D3D12_VIEWPORT s_Viewport { 0, 0, 16, 16, 0, 1 };
        D3D12_RECT s_Scissor { 0, 0, 16, 16 };

        const uint64_t s_DrawsBefore = CommandListHooks::GetDrawCount();

        p_Gpu.Begin();
        RecordUpload(p_Gpu, s_Target);

        p_Gpu.m_List->SetGraphicsRootSignature(s_Root.Get());
        p_Gpu.m_List->SetPipelineState(s_Pipeline.Get());
        p_Gpu.m_List->RSSetViewports(1, &s_Viewport);
        p_Gpu.m_List->RSSetScissorRects(1, &s_Scissor);
        p_Gpu.m_List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        p_Gpu.m_List->DrawInstanced(3, 1, 0, 0);
        Check(s_State.m_Calls == 0, "unarmed draw does not trigger the callback");

        CommandListHooks::SetArmed(true);
        p_Gpu.m_List->DrawInstanced(3, 1, 0, 0);
        CommandListHooks::SetArmed(false);

        Check(s_State.m_Calls == 1, "armed DrawInstanced triggers the callback");
        Check(s_State.m_SeenList == p_Gpu.m_List.Get(), "callback receives the recording command list");
        Check(s_State.m_SeenPipeline == s_Pipeline.Get(), "callback receives the tracked pipeline state");
        Check(s_State.m_Recorded, "flip recorded from inside a draw call");

        p_Gpu.m_List->DrawInstanced(3, 1, 0, 0);
        Check(s_State.m_Calls == 1, "draw after disarming does not trigger the callback");
        Check(CommandListHooks::GetDrawCount() - s_DrawsBefore == 3, "draw counter sees every draw");

        RecordReadback(p_Gpu, s_Target);
        p_Gpu.SubmitAndWait();

        Check(IsMirrored(s_Target), "image mirrored after flip in the middle of graphics work");
        Check(p_Gpu.m_Device->GetDeviceRemovedReason() == S_OK, "device still alive after restored pipeline draws");

        CommandListHooks::Uninstall();

        s_State.m_Calls = 0;
        p_Gpu.Begin();
        p_Gpu.m_List->Close();
        Check(s_State.m_Calls == 0, "hooks removed");
    }

    D3D12_RENDER_PASS_RENDER_TARGET_DESC RenderPassTarget(Gpu& p_Gpu, const Target& p_Target) {
        D3D12_DESCRIPTOR_HEAP_DESC s_HeapDesc {};
        s_HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        s_HeapDesc.NumDescriptors = 1;

        // Deliberately kept alive for the whole test run.
        ID3D12DescriptorHeap* s_Heap = nullptr;
        p_Gpu.m_Device->CreateDescriptorHeap(&s_HeapDesc, IID_PPV_ARGS(&s_Heap));

        const auto s_Handle = s_Heap->GetCPUDescriptorHandleForHeapStart();
        p_Gpu.m_Device->CreateRenderTargetView(p_Target.m_Texture.Get(), nullptr, s_Handle);

        D3D12_RENDER_PASS_RENDER_TARGET_DESC s_Pass {};
        s_Pass.cpuDescriptor = s_Handle;
        s_Pass.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        s_Pass.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        return s_Pass;
    }

    // Reproduces the crash seen in the game: a flip recorded inside a render pass makes Close fail.
    void TestFlipInsideRenderPassIsRejected(Gpu& p_Gpu, MirrorFlip& p_Flip) {
        Target s_Target = MakeTarget(p_Gpu, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 4);

        ComPtr<ID3D12GraphicsCommandList4> s_List4;
        p_Gpu.m_List.As(&s_List4);

        p_Gpu.Begin();
        RecordUpload(p_Gpu, s_Target);

        D3D12_RENDER_PASS_RENDER_TARGET_DESC s_Pass = RenderPassTarget(p_Gpu, s_Target);
        s_List4->BeginRenderPass(1, &s_Pass, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        p_Flip.Record(p_Gpu.m_List.Get(), s_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        s_List4->EndRenderPass();

        printf("[INFO] device state before that Close: 0x%08X\n", static_cast<uint32_t>(p_Gpu.m_Device->GetDeviceRemovedReason()));
        const HRESULT s_Result = p_Gpu.m_List->Close();
        printf("[INFO] Close after a flip inside a render pass returned 0x%08X\n", static_cast<uint32_t>(s_Result));
        printf("[INFO] device state after that Close: 0x%08X\n", static_cast<uint32_t>(p_Gpu.m_Device->GetDeviceRemovedReason()));
        Check(FAILED(s_Result), "a flip inside a render pass makes Close fail");
    }

    // Every target the game might use: the rehearsal must accept all of them and leave the list closable.
    void TestValidate(Gpu& p_Gpu, MirrorFlip& p_Flip, DXGI_FORMAT p_Format, UINT p_PixelSize, const char* p_Name) {
        Target s_Target = MakeTarget(p_Gpu, 800, 450, p_Format, p_PixelSize);

        std::string s_Error;
        const bool s_Valid = p_Flip.Validate(s_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &s_Error);
        if (!s_Valid) {
            printf("  %s\n", s_Error.c_str());
        }
        Check(s_Valid, std::string("rehearsal accepts ") + p_Name);

        p_Gpu.Begin();
        RecordUpload(p_Gpu, s_Target);
        p_Flip.Record(p_Gpu.m_List.Get(), s_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        RecordReadback(p_Gpu, s_Target);
        const HRESULT s_Closed = p_Gpu.m_List->Close();
        printf("[INFO] Close for %s: 0x%08X\n", p_Name, static_cast<uint32_t>(s_Closed));
        Check(SUCCEEDED(s_Closed), std::string("list closes with ") + p_Name);
    }

    void TestValidateRejects(Gpu& p_Gpu, MirrorFlip& p_Flip) {
        // A render target that is multisampled cannot be copied to a buffer.
        D3D12_HEAP_PROPERTIES s_Heap {};
        s_Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC s_Desc {};
        s_Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        s_Desc.Width = 64;
        s_Desc.Height = 64;
        s_Desc.DepthOrArraySize = 1;
        s_Desc.MipLevels = 1;
        s_Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s_Desc.SampleDesc.Count = 4;
        s_Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        ComPtr<ID3D12Resource> s_Texture;
        p_Gpu.m_Device->CreateCommittedResource(
            &s_Heap, D3D12_HEAP_FLAG_NONE, &s_Desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&s_Texture)
        );

        std::string s_Error;
        const bool s_Valid = p_Flip.Validate(s_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &s_Error);
        printf("[INFO] rehearsal said: %s\n", s_Error.c_str());
        Check(!s_Valid && !s_Error.empty(), "rehearsal rejects an unsupported target and explains why");
    }

    // Draws inside a render pass must never reach the flip, because it would make Close fail.
    void TestDrawInsideRenderPassIsIgnored(Gpu& p_Gpu, MirrorFlip& p_Flip) {
        Target s_Target = MakeTarget(p_Gpu, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 4);

        CallbackState s_State;
        s_State.m_Flip = &p_Flip;
        s_State.m_Target = s_Target.m_Texture.Get();

        CommandListHooks::Callbacks s_Callbacks;
        s_Callbacks.m_Context = &s_State;
        s_Callbacks.m_OnArmedSite = OnArmedDraw;
        CommandListHooks::Install(p_Gpu.m_Device.Get(), nullptr, s_Callbacks);

        ComPtr<ID3D12RootSignature> s_Root;
        const auto s_Pipeline = MakeDepthlessPipeline(p_Gpu.m_Device.Get(), s_Root, DXGI_FORMAT_R8G8B8A8_UNORM);

        ComPtr<ID3D12GraphicsCommandList4> s_List4;
        p_Gpu.m_List.As(&s_List4);

        D3D12_VIEWPORT s_Viewport { 0, 0, 16, 16, 0, 1 };
        D3D12_RECT s_Scissor { 0, 0, 16, 16 };
        D3D12_RENDER_PASS_RENDER_TARGET_DESC s_Pass = RenderPassTarget(p_Gpu, s_Target);

        p_Gpu.Begin();
        RecordUpload(p_Gpu, s_Target);

        p_Gpu.m_List->SetGraphicsRootSignature(s_Root.Get());
        p_Gpu.m_List->SetPipelineState(s_Pipeline.Get());
        p_Gpu.m_List->RSSetViewports(1, &s_Viewport);
        p_Gpu.m_List->RSSetScissorRects(1, &s_Scissor);
        p_Gpu.m_List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        CommandListHooks::SetArmed(true);
        s_List4->BeginRenderPass(1, &s_Pass, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        p_Gpu.m_List->DrawInstanced(3, 1, 0, 0);
        s_List4->EndRenderPass();
        CommandListHooks::SetArmed(false);

        Check(s_State.m_Calls == 0, "armed draw inside a render pass does not trigger the flip");

        const HRESULT s_Closed = p_Gpu.m_List->Close();
        printf("[INFO] Close after a draw inside a render pass: 0x%08X\n", static_cast<uint32_t>(s_Closed));
        Check(SUCCEEDED(s_Closed), "list with a render pass closes untouched");

        CommandListHooks::Uninstall();
    }

    struct PresentState {
        int m_Calls = 0;
        IDXGISwapChain* m_SwapChain = nullptr;
    };

    void OnPresentCallback(void* p_Context, IDXGISwapChain* p_SwapChain) {
        auto* s_State = static_cast<PresentState*>(p_Context);
        ++s_State->m_Calls;
        s_State->m_SwapChain = p_SwapChain;
    }

    // What the whole-frame mode depends on: the real present queue, the real back buffers and the present hook.
    void TestSwapChain(Gpu& p_Gpu, MirrorFlip& p_Flip) {
        WNDCLASSW s_Class {};
        s_Class.lpfnWndProc = DefWindowProcW;
        s_Class.hInstance = GetModuleHandleW(nullptr);
        s_Class.lpszClassName = L"MirrorFlipTest";
        RegisterClassW(&s_Class);
        HWND s_Window = CreateWindowW(
            L"MirrorFlipTest", L"MirrorFlipTest", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, s_Class.hInstance, nullptr
        );

        ComPtr<IDXGIFactory4> s_Factory;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&s_Factory));

        DXGI_SWAP_CHAIN_DESC1 s_Desc {};
        s_Desc.Width = 320;
        s_Desc.Height = 180;
        s_Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s_Desc.SampleDesc.Count = 1;
        s_Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        s_Desc.BufferCount = 2;
        s_Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> s_SwapChain1;
        const HRESULT s_Created = s_Factory->CreateSwapChainForHwnd(p_Gpu.m_Queue.Get(), s_Window, &s_Desc, nullptr, nullptr, &s_SwapChain1);
        Check(SUCCEEDED(s_Created), "swap chain created");
        if (FAILED(s_Created)) {
            return;
        }

        ComPtr<IDXGISwapChain3> s_SwapChain;
        s_SwapChain1.As(&s_SwapChain);

        ComPtr<ID3D12Resource> s_BackBuffer;
        s_SwapChain->GetBuffer(s_SwapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&s_BackBuffer));

        PresentState s_Present;
        CommandListHooks::Callbacks s_Callbacks;
        s_Callbacks.m_Context = &s_Present;
        s_Callbacks.m_OnPresent = OnPresentCallback;
        CommandListHooks::Install(p_Gpu.m_Device.Get(), s_SwapChain.Get(), s_Callbacks);

        p_Gpu.Begin();
        Check(p_Flip.Record(p_Gpu.m_List.Get(), s_BackBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT), "flip records on a real back buffer");
        const HRESULT s_Closed = p_Gpu.m_List->Close();
        printf("[INFO] Close for a real back buffer: 0x%08X\n", static_cast<uint32_t>(s_Closed));
        Check(SUCCEEDED(s_Closed), "list with a back buffer flip closes");
        ID3D12CommandList* s_Lists[] = { p_Gpu.m_List.Get() };
        Check(CommandListHooks::GetLastDirectQueue() == nullptr, "no queue known before anything was submitted");
        p_Gpu.m_Queue->ExecuteCommandLists(1, s_Lists);
        Check(CommandListHooks::GetLastDirectQueue() == p_Gpu.m_Queue.Get(), "ExecuteCommandLists hook finds the present queue");
        p_Gpu.m_Queue->Signal(p_Gpu.m_Fence.Get(), ++p_Gpu.m_Value);
        p_Gpu.m_Fence->SetEventOnCompletion(p_Gpu.m_Value, p_Gpu.m_Event);
        WaitForSingleObject(p_Gpu.m_Event, INFINITE);

        std::string s_RehearsalError;
        Check(p_Flip.Validate(s_BackBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &s_RehearsalError), "rehearsal accepts a real back buffer");

        s_BackBuffer.Reset();

        const HRESULT s_Result = s_SwapChain->Present(0, 0);
        Check(SUCCEEDED(s_Result) && s_Present.m_Calls == 1, "Present hook fires once");
        Check(s_Present.m_SwapChain == s_SwapChain.Get(), "Present hook receives the swap chain");

        s_SwapChain->Present(0, DXGI_PRESENT_TEST);
        Check(s_Present.m_Calls == 1, "DXGI_PRESENT_TEST is ignored");

        DXGI_PRESENT_PARAMETERS s_Parameters {};
        s_SwapChain->Present1(0, 0, &s_Parameters);
        Check(s_Present.m_Calls == 2, "Present1 hook fires");
        Check(p_Gpu.m_Device->GetDeviceRemovedReason() == S_OK, "device alive after presenting");

        p_Gpu.m_Queue->Signal(p_Gpu.m_Fence.Get(), ++p_Gpu.m_Value);
        p_Gpu.m_Fence->SetEventOnCompletion(p_Gpu.m_Value, p_Gpu.m_Event);
        WaitForSingleObject(p_Gpu.m_Event, INFINITE);

        CommandListHooks::Uninstall();
        DestroyWindow(s_Window);
    }

    // Closes a list holding a flip of a real back buffer, the way the game's own lists do.
    HRESULT CloseAfterFlippingBackBuffer(Gpu& p_Gpu, MirrorFlip& p_Flip, bool p_Current, bool p_PresentFirst, D3D12_RESOURCE_STATES p_State) {
        WNDCLASSW s_Class {};
        s_Class.lpfnWndProc = DefWindowProcW;
        s_Class.hInstance = GetModuleHandleW(nullptr);
        s_Class.lpszClassName = L"MirrorFlipTest2";
        RegisterClassW(&s_Class);
        HWND s_Window = CreateWindowW(
            L"MirrorFlipTest2", L"MirrorFlipTest2", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, s_Class.hInstance, nullptr
        );

        ComPtr<IDXGIFactory4> s_Factory;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&s_Factory));

        DXGI_SWAP_CHAIN_DESC1 s_Desc {};
        s_Desc.Width = 320;
        s_Desc.Height = 180;
        s_Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s_Desc.SampleDesc.Count = 1;
        s_Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        s_Desc.BufferCount = 2;
        s_Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> s_SwapChain1;
        s_Factory->CreateSwapChainForHwnd(p_Gpu.m_Queue.Get(), s_Window, &s_Desc, nullptr, nullptr, &s_SwapChain1);

        ComPtr<IDXGISwapChain3> s_SwapChain;
        s_SwapChain1.As(&s_SwapChain);

        if (p_PresentFirst) {
            s_SwapChain->Present(0, 0);
        }

        const UINT s_Index = s_SwapChain->GetCurrentBackBufferIndex();

        ComPtr<ID3D12Resource> s_BackBuffer;
        s_SwapChain->GetBuffer(p_Current ? s_Index : (s_Index + 1) % 2, IID_PPV_ARGS(&s_BackBuffer));

        p_Gpu.Begin();
        p_Flip.Record(p_Gpu.m_List.Get(), s_BackBuffer.Get(), p_State);
        const HRESULT s_Result = p_Gpu.m_List->Close();

        p_Gpu.m_Queue->Signal(p_Gpu.m_Fence.Get(), ++p_Gpu.m_Value);
        p_Gpu.m_Fence->SetEventOnCompletion(p_Gpu.m_Value, p_Gpu.m_Event);
        WaitForSingleObject(p_Gpu.m_Event, INFINITE);

        s_BackBuffer.Reset();
        s_SwapChain.Reset();
        s_SwapChain1.Reset();
        DestroyWindow(s_Window);
        UnregisterClassW(L"MirrorFlipTest2", s_Class.hInstance);

        return s_Result;
    }

    void TestRecentRenderTargets(Gpu& p_Gpu) {
        Target s_A = MakeTarget(p_Gpu, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
        Target s_B = MakeTarget(p_Gpu, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 4);

        CommandListHooks::Callbacks s_Callbacks;
        CommandListHooks::Install(p_Gpu.m_Device.Get(), nullptr, s_Callbacks);
        CommandListHooks::AdvanceFrame();

        ID3D12Resource* s_Recent[8] {};

        p_Gpu.Begin();

        const auto s_AToTarget = TextureBarrier(s_A.m_Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto s_BToTarget = TextureBarrier(s_B.m_Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto s_BToRead = TextureBarrier(s_B.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        p_Gpu.m_List->ResourceBarrier(1, &s_AToTarget);
        p_Gpu.m_List->ResourceBarrier(1, &s_BToTarget);

        int s_Count = CommandListHooks::GetRecentRenderTargets(s_Recent, 8);
        Check(s_Count == 2 && s_Recent[0] == s_B.m_Texture.Get() && s_Recent[1] == s_A.m_Texture.Get(), "newest render target comes first");

        p_Gpu.m_List->ResourceBarrier(1, &s_BToRead);
        s_Count = CommandListHooks::GetRecentRenderTargets(s_Recent, 8);
        Check(s_Count == 1 && s_Recent[0] == s_A.m_Texture.Get(), "a target moved out of the render target state is dropped");

        CommandListHooks::AdvanceFrame();
        Check(CommandListHooks::GetRecentRenderTargets(s_Recent, 8) == 0, "a new frame starts empty");

        p_Gpu.m_List->Close();
        CommandListHooks::Uninstall();
    }

    void MeasureGpuTime(Gpu& p_Gpu, MirrorFlip& p_Flip, UINT p_Width, UINT p_Height) {
        D3D12_QUERY_HEAP_DESC s_HeapDesc {};
        s_HeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        s_HeapDesc.Count = 2;

        ComPtr<ID3D12QueryHeap> s_Heap;
        p_Gpu.m_Device->CreateQueryHeap(&s_HeapDesc, IID_PPV_ARGS(&s_Heap));

        auto s_Results = MakeBuffer(p_Gpu.m_Device.Get(), D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
        Target s_Target = MakeTarget(p_Gpu, p_Width, p_Height, DXGI_FORMAT_R10G10B10A2_UNORM, 4);

        p_Gpu.Begin();
        RecordUpload(p_Gpu, s_Target);
        p_Gpu.SubmitAndWait();

        UINT64 s_Frequency = 1;
        p_Gpu.m_Queue->GetTimestampFrequency(&s_Frequency);

        double s_BestMs = 1e9;
        for (int s_Run = 0; s_Run < 5; ++s_Run) {
            p_Gpu.Begin();
            p_Gpu.m_List->EndQuery(s_Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            p_Flip.Record(p_Gpu.m_List.Get(), s_Target.m_Texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            p_Gpu.m_List->EndQuery(s_Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            p_Gpu.m_List->ResolveQueryData(s_Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, s_Results.Get(), 0);
            p_Gpu.SubmitAndWait();

            uint64_t* s_Mapped = nullptr;
            s_Results->Map(0, nullptr, reinterpret_cast<void**>(&s_Mapped));
            const double s_Ms = static_cast<double>(s_Mapped[1] - s_Mapped[0]) * 1000.0 / static_cast<double>(s_Frequency);
            s_Results->Unmap(0, nullptr);
            s_BestMs = (std::min)(s_BestMs, s_Ms);
        }

        printf("[INFO] GPU cost at %ux%u: %.3f ms\n", p_Width, p_Height, s_BestMs);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool s_DebugLayer = false;
    ComPtr<ID3D12Debug> s_Debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&s_Debug)))) {
        s_Debug->EnableDebugLayer();
        s_DebugLayer = true;
    }
    printf("[INFO] D3D12 debug layer: %s\n", s_DebugLayer ? "on" : "not installed");

    Gpu s_Gpu;
    ComPtr<IDXGIFactory6> s_Factory;
    ComPtr<IDXGIAdapter1> s_Adapter;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&s_Factory));
    s_Factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&s_Adapter));

    DXGI_ADAPTER_DESC1 s_AdapterDesc {};
    s_Adapter->GetDesc1(&s_AdapterDesc);
    printf("[INFO] Adapter: %ls\n", s_AdapterDesc.Description);

    if (FAILED(D3D12CreateDevice(s_Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s_Gpu.m_Device)))) {
        printf("[FAIL] could not create a device\n");
        return 2;
    }

    ComPtr<ID3D12InfoQueue> s_InfoQueue;
    if (s_DebugLayer && SUCCEEDED(s_Gpu.m_Device.As(&s_InfoQueue))) {
        s_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    }

    D3D12_COMMAND_QUEUE_DESC s_QueueDesc {};
    s_QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    s_Gpu.m_Device->CreateCommandQueue(&s_QueueDesc, IID_PPV_ARGS(&s_Gpu.m_Queue));
    s_Gpu.m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_Gpu.m_Allocator));
    s_Gpu.m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_Gpu.m_Allocator.Get(), nullptr, IID_PPV_ARGS(&s_Gpu.m_List));
    s_Gpu.m_List->Close();
    s_Gpu.m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_Gpu.m_Fence));
    s_Gpu.m_Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    MirrorFlip s_Flip;
    std::string s_Error;
    Check(s_Flip.Initialize(s_Gpu.m_Device.Get(), &s_Error), "initialize " + s_Error);

    TestFlip(s_Gpu, s_Flip, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 4, "1920x1080 RGBA8");
    TestFlip(s_Gpu, s_Flip, 2560, 1440, DXGI_FORMAT_R10G10B10A2_UNORM, 4, "2560x1440 RGB10A2");
    TestFlip(s_Gpu, s_Flip, 1919, 1079, DXGI_FORMAT_B8G8R8A8_UNORM, 4, "odd size 1919x1079 BGRA8");
    TestFlip(s_Gpu, s_Flip, 1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "1280x720 RGBA16F");
    TestFlip(s_Gpu, s_Flip, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, "1x1");
    TestFlip(s_Gpu, s_Flip, 3840, 2160, DXGI_FORMAT_R8G8B8A8_UNORM, 4, "growing the buffer to 4K");

    TestHooks(s_Gpu, s_Flip);
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_R8G8B8A8_UNORM, 4, "RGBA8");
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, "RGBA8 sRGB");
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, "RGBA8 typeless");
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 4, "BGRA8 sRGB");
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_R10G10B10A2_TYPELESS, 4, "RGB10A2 typeless");
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_R16G16B16A16_TYPELESS, 8, "RGBA16 typeless");
    TestValidate(s_Gpu, s_Flip, DXGI_FORMAT_R11G11B10_FLOAT, 4, "R11G11B10");
    TestValidateRejects(s_Gpu, s_Flip);
    TestSwapChain(s_Gpu, s_Flip);
    TestRecentRenderTargets(s_Gpu);
    TestDrawInsideRenderPassIsIgnored(s_Gpu, s_Flip);

    MeasureGpuTime(s_Gpu, s_Flip, 2560, 1440);
    MeasureGpuTime(s_Gpu, s_Flip, 3840, 2160);

    // Deliberately invalid, so these run last and cannot affect the other tests.
    Check(
        SUCCEEDED(CloseAfterFlippingBackBuffer(s_Gpu, s_Flip, true, true, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "flip on the current back buffer closes after a present"
    );

    TestFlipInsideRenderPassIsRejected(s_Gpu, s_Flip);

    int s_ValidationErrors = 0;
    if (s_InfoQueue) {
        const UINT64 s_Count = s_InfoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < s_Count; ++i) {
            SIZE_T s_Size = 0;
            s_InfoQueue->GetMessage(i, nullptr, &s_Size);
            std::vector<char> s_Storage(s_Size);
            auto* s_Message = reinterpret_cast<D3D12_MESSAGE*>(s_Storage.data());
            s_InfoQueue->GetMessage(i, s_Message, &s_Size);
            if (s_Message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                printf("  D3D12: %s\n", s_Message->pDescription);
                ++s_ValidationErrors;
            }
        }
        Check(s_ValidationErrors == 0, "no D3D12 validation errors");
    }

    return Finish();
}
