// Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/**
 * Mirrors a texture horizontally on the GPU, recorded into any open direct command list.
 *
 * The texture is copied to a raw buffer, a compute shader swaps the pixels of each row in place,
 * and the buffer is copied back. Working on a buffer makes the pass independent of the texture format
 * and needs no descriptors, so the only command list state it changes is the pipeline state and the
 * compute root signature. Graphics root bindings, render targets, viewports and descriptor heaps stay untouched.
 */
class MirrorFlip {
public:
    bool Initialize(ID3D12Device* p_Device, std::string* p_Error);

    /**
     * Records the flip. Returns false without recording anything if the target is unsupported.
     * The target must be in p_State, and is back in p_State afterwards.
     * The caller must restore the pipeline state and compute root signature it needs.
     */
    bool Record(
        ID3D12GraphicsCommandList* p_List, ID3D12Resource* p_Target, D3D12_RESOURCE_STATES p_State
    );

    /**
     * Rehearses the flip for this target on a private command list that is closed but never executed.
     * A command list that fails to close makes the engine abort, so this has to pass before the engine's list
     * is touched. The result is cached per target. On failure p_Error describes the target.
     */
    bool Validate(ID3D12Resource* p_Target, D3D12_RESOURCE_STATES p_State, std::string* p_Error);

private:
    bool RecordLocked(ID3D12GraphicsCommandList* p_List, ID3D12Resource* p_Target, D3D12_RESOURCE_STATES p_State);
    bool EnsureBuffer(uint64_t p_Size);

    struct RetiredBuffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Buffer;
        uint64_t m_RetiredAt;
    };

    Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_Pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Buffer;
    uint64_t m_BufferSize = 0;

    // A resized buffer can still be in use by frames in flight, so it is released later.
    std::vector<RetiredBuffer> m_Retired;
    uint64_t m_RecordCount = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_ValidationAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_ValidationList;

    // One entry per back buffer, which take turns every frame.
    struct ValidatedTarget {
        ID3D12Resource* m_Target;
        D3D12_RESOURCE_DESC m_Desc;
    };

    std::vector<ValidatedTarget> m_Validated;

    std::mutex m_Mutex;
};
