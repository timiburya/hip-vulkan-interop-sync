// repro_d3d12_fence.hip - HIP<->Vulkan ping-pong synchronised with D3D12 shared fences (ROCm issue #3909).
// Two D3D12 fences are imported into Vulkan (timeline VkSemaphore, D3D12_FENCE_BIT) and HIP
// (hipExternalSemaphoreHandleTypeD3D12Fence). HIP and Vulkan alternate on a shared buffer pair for N bounces;
// the final particle checksum is compared with a plain-HIP reference to detect missing or late synchronisation.
#include "repro_common.h"

// Create D3D12 fence semaphores
static bool CreateD3D12Sems(hipExternalSemaphore_t* hipSem, VkSemaphore* vkSem, HANDLE fh) {
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    CVK(vkCreateSemaphore(g_vkDev, &sci, nullptr, vkSem), __LINE__);
    auto pfn = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(g_vkDev, "vkImportSemaphoreWin32HandleKHR");
    if (!pfn) return false;
    VkImportSemaphoreWin32HandleInfoKHR ii;
    ii.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    ii.pNext = nullptr;
    ii.semaphore = *vkSem;
    ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    ii.handle = fh;
    CVK(pfn(g_vkDev, &ii), __LINE__);
    hipExternalSemaphoreHandleDesc hd{};
    hd.type = hipExternalSemaphoreHandleTypeD3D12Fence;
    hd.flags = 0;
    hd.handle.win32.handle = fh;
    CHIP(hipImportExternalSemaphore(hipSem, &hd), __LINE__);
    return true;
}

int main(int argc, char** argv) {
    Options opt = ParseArgs(argc, argv, "repro_d3d12_fence");
    InitAll(true);
    SetupBuffers(opt);

    HANDLE h1 = nullptr, h2 = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> f1, f2;
    if (FAILED(g_d3d12Dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f1)))) { fprintf(stderr, "CreateFence failed\n"); return 1; }
    if (FAILED(g_d3d12Dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f2)))) { fprintf(stderr, "CreateFence failed\n"); return 1; }
    if (FAILED(g_d3d12Dev->CreateSharedHandle(f1.Get(), nullptr, GENERIC_ALL, nullptr, &h1)) ||
        FAILED(g_d3d12Dev->CreateSharedHandle(f2.Get(), nullptr, GENERIC_ALL, nullptr, &h2))) { fprintf(stderr, "CreateSharedHandle failed\n"); return 1; }
    if (!CreateD3D12Sems(&g_hipHipDone, &g_vkHipDone, h1) || !CreateD3D12Sems(&g_hipVkDone, &g_vkVkDone, h2)) { fprintf(stderr, "D3D12 sem import failed\n"); return 2; }
    CloseHandle(h1); CloseHandle(h2);
    printf("D3D12 fence semaphores imported\n");

    return RunPingPong(opt, true);
}
