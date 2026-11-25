// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/MirrorFlip.h"

#include "core/Log.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <format>

using Microsoft::WRL::ComPtr;

namespace {
    constexpr UINT c_ThreadsPerGroup = 64;
    constexpr uint64_t c_RetireAfterRecords = 16;

    Log::Budget g_BufferFailureLines(4);

    const char c_ShaderSource[] = R"(
cbuffer Constants : register(b0) {
    uint g_Width;
    uint g_Height;
    uint g_RowPitch;
    uint g_PixelSize;
};

RWByteAddressBuffer g_Pixels : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 p_Id : SV_DispatchThreadID) {
    if (p_Id.x >= g_Width / 2 || p_Id.y >= g_Height) {
        return;
    }

    const uint s_Row = p_Id.y * g_RowPitch;
    const uint s_Left = s_Row + p_Id.x * g_PixelSize;
    const uint s_Right = s_Row + (g_Width - 1 - p_Id.x) * g_PixelSize;

    for (uint i = 0; i < g_PixelSize; i += 4) {
        const uint s_A = g_Pixels.Load(s_Left + i);
        g_Pixels.Store(s_Left + i, g_Pixels.Load(s_Right + i));
        g_Pixels.Store(s_Right + i, s_A);
    }
}
)";

    struct Constants {
        UINT m_Width;
        UINT m_Height;
        UINT m_RowPitch;
        UINT m_PixelSize;
    };

    D3D12_RESOURCE_BARRIER Transition(
        ID3D12Resource* p_Resource, D3D12_RESOURCE_STATES p_Before, D3D12_RESOURCE_STATES p_After, UINT p_Subresource
    ) {
        D3D12_RESOURCE_BARRIER s_Barrier {};
        s_Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        s_Barrier.Transition.pResource = p_Resource;
        s_Barrier.Transition.Subresource = p_Subresource;
        s_Barrier.Transition.StateBefore = p_Before;
        s_Barrier.Transition.StateAfter = p_After;
        return s_Barrier;
    }
}

bool MirrorFlip::Initialize(ID3D12Device* p_Device, std::string* p_Error) {
    std::scoped_lock s_Lock(m_Mutex);

    const auto s_Fail = [&](const char* p_Step, HRESULT p_Result) {
        if (p_Error) {
            *p_Error = std::format("{} failed with {:#x}", p_Step, static_cast<uint32_t>(p_Result));
        }
        return false;
    };

    m_Device = p_Device;

    ComPtr<ID3DBlob> s_Shader;
    ComPtr<ID3DBlob> s_Errors;
    HRESULT s_Result = D3DCompile(
        c_ShaderSource, sizeof(c_ShaderSource) - 1, "MirrorFlip", nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &s_Shader, &s_Errors
    );

    if (FAILED(s_Result)) {
        if (p_Error && s_Errors) {
            *p_Error = static_cast<const char*>(s_Errors->GetBufferPointer());
            return false;
        }
        return s_Fail("D3DCompile", s_Result);
    }

    D3D12_ROOT_PARAMETER s_Params[2] {};
    s_Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    s_Params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    s_Params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    s_Params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    s_Params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC s_RootDesc {};
    s_RootDesc.NumParameters = 2;
    s_RootDesc.pParameters = s_Params;

    ComPtr<ID3DBlob> s_RootBlob;
    s_Result = D3D12SerializeRootSignature(&s_RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &s_RootBlob, &s_Errors);
    if (FAILED(s_Result)) {
        return s_Fail("D3D12SerializeRootSignature", s_Result);
    }

    s_Result = m_Device->CreateRootSignature(
        0, s_RootBlob->GetBufferPointer(), s_RootBlob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)
    );
    if (FAILED(s_Result)) {
        return s_Fail("CreateRootSignature", s_Result);
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC s_PipelineDesc {};
    s_PipelineDesc.pRootSignature = m_RootSignature.Get();
    s_PipelineDesc.CS = { s_Shader->GetBufferPointer(), s_Shader->GetBufferSize() };

    s_Result = m_Device->CreateComputePipelineState(&s_PipelineDesc, IID_PPV_ARGS(&m_Pipeline));
    if (FAILED(s_Result)) {
        return s_Fail("CreateComputePipelineState", s_Result);
    }

    s_Result = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_ValidationAllocator));
    if (FAILED(s_Result)) {
        return s_Fail("CreateCommandAllocator", s_Result);
    }

    s_Result = m_Device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_ValidationAllocator.Get(), nullptr, IID_PPV_ARGS(&m_ValidationList)
    );
    if (FAILED(s_Result)) {
        return s_Fail("CreateCommandList", s_Result);
    }

    m_ValidationList->Close();

    return true;
}

bool MirrorFlip::Validate(ID3D12Resource* p_Target, D3D12_RESOURCE_STATES p_State, std::string* p_Error) {
    std::scoped_lock s_Lock(m_Mutex);

    if (!m_ValidationList || !p_Target) {
        return false;
    }

    const D3D12_RESOURCE_DESC s_Desc = p_Target->GetDesc();

    for (const auto& s_Entry : m_Validated) {
        if (s_Entry.m_Target == p_Target && memcmp(&s_Desc, &s_Entry.m_Desc, sizeof(s_Desc)) == 0) {
            return true;
        }
    }

    const auto s_Fail = [&](const char* p_Reason, HRESULT p_Result) {
        if (p_Error) {
            *p_Error = std::format(
                "{} ({:#x}): {}x{} format {} flags {:#x} samples {} array {} mips {} layout {}",
                p_Reason, static_cast<uint32_t>(p_Result), s_Desc.Width, s_Desc.Height, static_cast<int>(s_Desc.Format),
                static_cast<int>(s_Desc.Flags), s_Desc.SampleDesc.Count, s_Desc.DepthOrArraySize, s_Desc.MipLevels,
                static_cast<int>(s_Desc.Layout)
            );
        }
        return false;
    };

    // Never executed, so resetting the allocator is always legal.
    m_ValidationAllocator->Reset();
    m_ValidationList->Reset(m_ValidationAllocator.Get(), nullptr);

    const bool s_Recorded = RecordLocked(m_ValidationList.Get(), p_Target, p_State);
    const HRESULT s_Result = m_ValidationList->Close();

    if (!s_Recorded) {
        return s_Fail("target not supported", S_OK);
    }

    if (FAILED(s_Result)) {
        return s_Fail("rehearsal did not close", s_Result);
    }

    std::erase_if(m_Validated, [&](const ValidatedTarget& p_Entry) { return p_Entry.m_Target == p_Target; });
    m_Validated.push_back({ p_Target, s_Desc });
    return true;
}

bool MirrorFlip::EnsureBuffer(uint64_t p_Size) {
    if (m_Buffer && m_BufferSize >= p_Size) {
        return true;
    }

    if (m_Buffer) {
        m_Retired.push_back({ std::move(m_Buffer), m_RecordCount });
    }

    D3D12_HEAP_PROPERTIES s_Heap {};
    s_Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC s_Desc {};
    s_Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    s_Desc.Width = p_Size;
    s_Desc.Height = 1;
    s_Desc.DepthOrArraySize = 1;
    s_Desc.MipLevels = 1;
    s_Desc.SampleDesc.Count = 1;
    s_Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    s_Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (const HRESULT s_Result = m_Device->CreateCommittedResource(
        &s_Heap, D3D12_HEAP_FLAG_NONE, &s_Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Buffer)
    ); FAILED(s_Result)) {
        if (g_BufferFailureLines.Take()) {
            Log::Error("render", "Creating the {} byte flip buffer failed with {:#x}.", p_Size, static_cast<uint32_t>(s_Result));
        }

        m_BufferSize = 0;
        return false;
    }

    m_BufferSize = p_Size;
    return true;
}

bool MirrorFlip::Record(
    ID3D12GraphicsCommandList* p_List, ID3D12Resource* p_Target, D3D12_RESOURCE_STATES p_State
) {
    std::scoped_lock s_Lock(m_Mutex);
    return RecordLocked(p_List, p_Target, p_State);
}

bool MirrorFlip::RecordLocked(
    ID3D12GraphicsCommandList* p_List, ID3D12Resource* p_Target, D3D12_RESOURCE_STATES p_State
) {
    if (!m_Pipeline || !p_List || !p_Target) {
        return false;
    }

    const D3D12_RESOURCE_DESC s_Desc = p_Target->GetDesc();

    if (s_Desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        s_Desc.SampleDesc.Count != 1 ||
        s_Desc.DepthOrArraySize != 1) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_Footprint {};
    UINT s_NumRows = 0;
    UINT64 s_RowSize = 0;
    UINT64 s_TotalSize = 0;
    m_Device->GetCopyableFootprints(&s_Desc, 0, 1, 0, &s_Footprint, &s_NumRows, &s_RowSize, &s_TotalSize);

    const UINT s_Width = s_Footprint.Footprint.Width;
    const UINT s_Height = s_Footprint.Footprint.Height;

    // Rejects block-compressed and planar formats, whose rows are not whole pixels.
    if (s_Width == 0 || s_NumRows != s_Height || s_RowSize % s_Width != 0) {
        return false;
    }

    const UINT s_PixelSize = static_cast<UINT>(s_RowSize / s_Width);

    if (s_PixelSize % 4 != 0 || s_PixelSize == 0) {
        return false;
    }

    if (!EnsureBuffer(s_TotalSize)) {
        return false;
    }

    ++m_RecordCount;
    std::erase_if(m_Retired, [this](const RetiredBuffer& p_Retired) {
        return m_RecordCount - p_Retired.m_RetiredAt > c_RetireAfterRecords;
    });

    ID3D12Resource* s_Buffer = m_Buffer.Get();

    D3D12_TEXTURE_COPY_LOCATION s_TextureLocation {};
    s_TextureLocation.pResource = p_Target;
    s_TextureLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_TextureLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION s_BufferLocation {};
    s_BufferLocation.pResource = s_Buffer;
    s_BufferLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s_BufferLocation.PlacedFootprint = s_Footprint;

    constexpr UINT c_Whole = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    const D3D12_RESOURCE_BARRIER s_ToReadTarget[] = {
        Transition(p_Target, p_State, D3D12_RESOURCE_STATE_COPY_SOURCE, 0),
        Transition(s_Buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST, c_Whole),
    };
    p_List->ResourceBarrier(2, s_ToReadTarget);
    p_List->CopyTextureRegion(&s_BufferLocation, 0, 0, 0, &s_TextureLocation, nullptr);

    const D3D12_RESOURCE_BARRIER s_ToCompute =
        Transition(s_Buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, c_Whole);
    p_List->ResourceBarrier(1, &s_ToCompute);

    const Constants s_Constants { s_Width, s_Height, s_Footprint.Footprint.RowPitch, s_PixelSize };

    p_List->SetPipelineState(m_Pipeline.Get());
    p_List->SetComputeRootSignature(m_RootSignature.Get());
    p_List->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &s_Constants, 0);
    p_List->SetComputeRootUnorderedAccessView(1, s_Buffer->GetGPUVirtualAddress());
    p_List->Dispatch((s_Width / 2 + c_ThreadsPerGroup - 1) / c_ThreadsPerGroup, s_Height, 1);

    const D3D12_RESOURCE_BARRIER s_ToWriteTarget[] = {
        Transition(s_Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, c_Whole),
        Transition(p_Target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, 0),
    };
    p_List->ResourceBarrier(2, s_ToWriteTarget);
    p_List->CopyTextureRegion(&s_TextureLocation, 0, 0, 0, &s_BufferLocation, nullptr);

    const D3D12_RESOURCE_BARRIER s_Restore[] = {
        Transition(s_Buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON, c_Whole),
        Transition(p_Target, D3D12_RESOURCE_STATE_COPY_DEST, p_State, 0),
    };
    p_List->ResourceBarrier(2, s_Restore);

    return true;
}
