// repro_opaque.hip - HIP<->Vulkan ping-pong synchronised with exportable Vulkan timeline semaphores (ROCm issue #3909).
// Built as repro_opaque_win32 on Windows (opaque Win32 handles) and repro_opaque_fd on Linux (opaque fds).
// Step 1: importing a Vulkan timeline semaphore handle as hipExternalSemaphoreHandleTypeTimelineSemaphore{Win32,Fd} is attempted
// and its result printed. Step 2: the same handles are imported as hipExternalSemaphoreHandleTypeOpaque{Win32,Fd} and used for
// the ping-pong; the final particle checksum is compared with a plain-HIP reference.
#include "repro_common.h"

#ifdef _WIN32
#define PROG_NAME "repro_opaque_win32"
#define OPAQUE_NAME "OpaqueWin32"
#define TIMELINE_NAME "TimelineSemaphoreWin32"
#else
#define PROG_NAME "repro_opaque_fd"
#define OPAQUE_NAME "OpaqueFd"
#define TIMELINE_NAME "TimelineSemaphoreFd"
#endif

// Create exportable opaque (Win32 handle / fd) Vulkan timeline semaphore
static bool CreateOpaqueSem(VkSemaphore* sem, ExtHandle* h) {
#ifdef _WIN32
    const VkExternalSemaphoreHandleTypeFlagBits ht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    const VkExternalSemaphoreHandleTypeFlagBits ht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    VkExportSemaphoreCreateInfo ex{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, ht};
    VkSemaphoreTypeCreateInfo tc{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &ex, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tc};
    CVK(vkCreateSemaphore(g_vkDev, &sc, nullptr, sem), __LINE__);
#ifdef _WIN32
    auto pfn = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(g_vkDev, "vkGetSemaphoreWin32HandleKHR");
    if (!pfn) return false;
    VkSemaphoreGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, *sem, ht};
#else
    auto pfn = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(g_vkDev, "vkGetSemaphoreFdKHR");
    if (!pfn) return false;
    VkSemaphoreGetFdInfoKHR hi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, *sem, ht};
#endif
    CVK(pfn(g_vkDev, &hi, h), __LINE__);
    return true;
}

// Import semaphore handle to HIP with the given HIP handle type
static hipError_t ImportSem(hipExternalSemaphore_t* hipSem, ExtHandle h, hipExternalSemaphoreHandleType type) {
    hipExternalSemaphoreHandleDesc hd{};
    hd.type = type;
    hd.flags = 0;
#ifdef _WIN32
    hd.handle.win32.handle = h;
#else
    hd.handle.fd = h;   // on success HIP owns the fd
#endif
    return hipImportExternalSemaphore(hipSem, &hd);
}

// Import opaque semaphore to HIP
static bool ImportOpaqueSem(hipExternalSemaphore_t* hipSem, ExtHandle h) {
#ifdef _WIN32
    return ImportSem(hipSem, h, hipExternalSemaphoreHandleTypeOpaqueWin32) == hipSuccess;
#else
    return ImportSem(hipSem, h, hipExternalSemaphoreHandleTypeOpaqueFd) == hipSuccess;
#endif
}

int main(int argc, char** argv) {
    Options opt = ParseArgs(argc, argv, PROG_NAME);
    InitAll(false);
    SetupBuffers(opt);

    ExtHandle h1 = EXT_HANDLE_NONE, h2 = EXT_HANDLE_NONE;
    if (!CreateOpaqueSem(&g_vkHipDone, &h1) || !CreateOpaqueSem(&g_vkVkDone, &h2)) { fprintf(stderr, "Opaque sem creation failed\n"); return 2; }

    // Step 1: Timeline semaphore handle-type import
    {
#ifdef _WIN32
        ExtHandle probe = h1;
        const hipExternalSemaphoreHandleType tt = hipExternalSemaphoreHandleTypeTimelineSemaphoreWin32;
#else
        ExtHandle probe = dup(h1);   // HIP takes ownership of an imported fd; probe a copy so h1 stays usable
        const hipExternalSemaphoreHandleType tt = hipExternalSemaphoreHandleTypeTimelineSemaphoreFd;
#endif
        hipExternalSemaphore_t tmp = nullptr;
        hipError_t e = ImportSem(&tmp, probe, tt);
        printf(TIMELINE_NAME " import: %s\n", HipErrorName(e));
        if (e == hipSuccess) {
            printf("note: " TIMELINE_NAME " import unexpectedly succeeded; destroying it and continuing with " OPAQUE_NAME "\n");
            hipDestroyExternalSemaphore(tmp);
        }
#ifndef _WIN32
        else if (probe >= 0) close(probe);
#endif
        hipGetLastError(); // clear sticky error
    }

    // Step 2: Opaque import
    if (!ImportOpaqueSem(&g_hipHipDone, h1) || !ImportOpaqueSem(&g_hipVkDone, h2)) { fprintf(stderr, "Opaque sem import failed\n"); return 2; }
    printf(OPAQUE_NAME " import of Vulkan timeline semaphores: OK\n");
#ifdef _WIN32
    CloseHandle(h1); CloseHandle(h2);
#endif

    return RunPingPong(opt, false);
}
