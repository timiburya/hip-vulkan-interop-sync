// repro_common.h - shared code for the HIP<->Vulkan(/D3D12) semaphore repro programs (ROCm issue #3909)
#pragma once
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <hip/hip_runtime.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl.h>
#else
#include <unistd.h>   // _exit
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>
#include <thread>
#include <vector>

// Platform glue: external handle type (Win32 HANDLE / POSIX fd), exported memory handle type, sleep
#ifdef _WIN32
typedef HANDLE ExtHandle;
static const ExtHandle EXT_HANDLE_NONE = nullptr;
static inline bool ExtHandleValid(ExtHandle h) { return h != nullptr; }
#define EXT_MEM_HANDLE_TYPE VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT_KHR
#else
typedef int ExtHandle;
static const ExtHandle EXT_HANDLE_NONE = -1;
static inline bool ExtHandleValid(ExtHandle h) { return h >= 0; }
#define EXT_MEM_HANDLE_TYPE VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
static inline void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// --- RNG from ZeroCopyPhysics.h (verbatim) ---
struct ZC_Rng { uint64_t s; };
__device__ __forceinline__ uint64_t ZC_Mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
__device__ __forceinline__ void ZC_RngInit(uint64_t seed, uint64_t seq, uint64_t offset, ZC_Rng* r) {
    r->s = ZC_Mix64(seed ^ ZC_Mix64(seq * 0x9E3779B97F4A7C15ull + ZC_Mix64(offset + 0x632BE59BD9B4E019ull)));
}
__device__ __forceinline__ float ZC_Uniform(ZC_Rng* r) {
    r->s += 0x9E3779B97F4A7C15ull;
    const uint32_t x = (uint32_t)(ZC_Mix64(r->s) >> 40);
    return (float)(x + 1u) * (1.0f / 16777216.0f);
}
__device__ void ZC_GetIsotropicDir(ZC_Rng* rng, float* dir) {
    float z = 1.0f - 2.0f * ZC_Uniform(rng);
    float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
    float sp, cp; sincospif(2.0f * ZC_Uniform(rng), &sp, &cp);
    dir[0] = r * cp; dir[1] = r * sp; dir[2] = z;
}
__device__ __forceinline__ void ZC_ElasticScatter(float A, ZC_Rng* rng, float& E, float* dir, float cos_cm, float& outRatio) {
    const float denom = sqrtf(A*A + 2.0f*A*cos_cm + 1.0f);
    const float cos_lab = (A * cos_cm + 1.0f) / denom;
    const float sin_lab = sqrtf(fmaxf(0.0f, 1.0f - cos_lab * cos_lab));
    const float phiRev = 2.0f * ZC_Uniform(rng);
    outRatio = (A*A + 2.0f*A*cos_cm + 1.0f) / ((A + 1.0f) * (A + 1.0f));
    E *= outRatio;
    if (fabsf(dir[0]) < 0.9f) { float u[3] = {0, -dir[2], dir[1]}; float v[3] = {dir[1]*u[2]-dir[2]*u[1], dir[2]*u[0]-dir[0]*u[2], dir[0]*u[1]-dir[1]*u[0]};
        float ul = rsqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); u[0]*=ul; u[1]*=ul; u[2]*=ul;
        float vl = rsqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); v[0]*=vl; v[1]*=vl; v[2]*=vl;
        float sp2, cp2; sincospif(phiRev, &sp2, &cp2);
        dir[0] = cos_lab*dir[0] + sin_lab*(cp2*u[0] + sp2*v[0]);
        dir[1] = cos_lab*dir[1] + sin_lab*(cp2*u[1] + sp2*v[1]);
        dir[2] = cos_lab*dir[2] + sin_lab*(cp2*u[2] + sp2*v[2]);
    } else { float u[3] = {-dir[2], 0, dir[0]}; float v[3] = {dir[1]*u[2]-dir[2]*u[1], dir[2]*u[0]-dir[0]*u[2], dir[0]*u[1]-dir[1]*u[0]};
        float ul = rsqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); u[0]*=ul; u[1]*=ul; u[2]*=ul;
        float vl = rsqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); v[0]*=vl; v[1]*=vl; v[2]*=vl;
        float sp2, cp2; sincospif(phiRev, &sp2, &cp2);
        dir[0] = cos_lab*dir[0] + sin_lab*(cp2*u[0] + sp2*v[0]);
        dir[1] = cos_lab*dir[1] + sin_lab*(cp2*u[1] + sp2*v[1]);
        dir[2] = cos_lab*dir[2] + sin_lab*(cp2*u[2] + sp2*v[2]);
    }
}

// Particle and constants
#define A_MAT 12.0f
#define SIGMA_EL 0.385f
#define SIGMA_CAP 0.0003f
#define SIGMA_T (SIGMA_EL + SIGMA_CAP)
#define E0 2000000.0f
#define E_MIN 1e-5f
struct Particle { float x,y,z; float dir[3]; float E; float weight; uint32_t alive; };
struct Checksum { unsigned long long alive_count; unsigned long long logE_sum; unsigned long long pos_sum; };

// Global HIP/VK/D3D12 state
static VkInstance g_vkInst; static VkPhysicalDevice g_vkPhys; static VkDevice g_vkDev; static VkQueue g_vkQueue;
static uint32_t g_vkQF;
#ifdef _WIN32
static ID3D12Device* g_d3d12Dev = nullptr;
#endif
static hipStream_t g_stream = nullptr;
static VkSemaphore g_vkHipDone = VK_NULL_HANDLE, g_vkVkDone = VK_NULL_HANDLE;
static hipExternalSemaphore_t g_hipHipDone = nullptr, g_hipVkDone = nullptr;
static VkBuffer g_bufA = VK_NULL_HANDLE, g_bufB = VK_NULL_HANDLE;
static VkDeviceMemory g_memA = VK_NULL_HANDLE, g_memB = VK_NULL_HANDLE;
static ExtHandle g_kmtA = EXT_HANDLE_NONE, g_kmtB = EXT_HANDLE_NONE;
static void* g_d_bufA = nullptr, *g_d_bufB = nullptr;
static VkCommandPool g_cmdPool = VK_NULL_HANDLE;
static VkCommandBuffer g_cmdBuf = VK_NULL_HANDLE;
static VkFence g_vkFence = VK_NULL_HANDLE;

// Error macros
#define CVK(cmd, ln) do { VkResult r = (cmd); if (r != VK_SUCCESS) { fprintf(stderr, "VK %d ln %d\n", r, ln); exit(1); } } while(0)
#define CHIP(cmd, ln) do { hipError_t e = (cmd); if (e != hipSuccess) { fprintf(stderr, "HIP %s ln %d\n", hipGetErrorName(e), ln); exit(2); } } while(0)

// 4-tier memory picker: prefer DEVICE_LOCAL for exportable
static uint32_t PickMemType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(pd, &props);
    VkMemoryPropertyFlags cands[] = { want };
    for (auto c : cands) for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u<<i)) && ((props.memoryTypes[i].propertyFlags & c) == c)) return i;
    fprintf(stderr, "PickMemType: no type matching bits %x and flags %x\n", bits, want);
    exit(1);
}

// LUID match (Windows / D3D12 only)
#ifdef _WIN32
static bool LUIDMatch(VkPhysicalDevice vkpd, IDXGIAdapter1** outAd, ID3D12Device** outD3D) {
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vkGetPhysicalDeviceProperties2(vkpd, &p2);
    if (!idp.deviceLUIDValid) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory4> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    for (UINT i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, idp.deviceLUID, VK_LUID_SIZE) == 0) {
            if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(outD3D)))) { *outAd = a.Detach(); return true; }
        }
    }
    return false;
}
#endif

// Create exportable Vulkan buffer
static bool CreateExpBuf(size_t sz, VkBuffer* b, VkDeviceMemory* m, ExtHandle* h) {
    VkExternalMemoryBufferCreateInfo ex{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
        EXT_MEM_HANDLE_TYPE};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ex, 0, sz,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    CVK(vkCreateBuffer(g_vkDev, &bi, nullptr, b), __LINE__);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vkDev, *b, &mr);
    uint32_t mt = PickMemType(g_vkPhys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return false;
    VkExportMemoryAllocateInfo exma{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr,
        EXT_MEM_HANDLE_TYPE};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exma, mr.size, mt};
    CVK(vkAllocateMemory(g_vkDev, &mai, nullptr, m), __LINE__);
    CVK(vkBindBufferMemory(g_vkDev, *b, *m, 0), __LINE__);
#ifdef _WIN32
    auto pfn = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(g_vkDev, "vkGetMemoryWin32HandleKHR");
    if (!pfn) return false;
    VkMemoryGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, *m,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT_KHR};
#else
    auto pfn = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_vkDev, "vkGetMemoryFdKHR");
    if (!pfn) return false;
    VkMemoryGetFdInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, *m,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
#endif
    CVK(pfn(g_vkDev, &hi, h), __LINE__);
    return ExtHandleValid(*h);
}

// Import buffer to HIP
// (Linux: HIP takes ownership of the fd on successful import, so it is not closed here)
static bool ImportBuf(ExtHandle h, size_t sz, void** ptr) {
    hipExternalMemoryHandleDesc md{};
    md.size = (unsigned long long)sz;
#ifdef _WIN32
    md.type = hipExternalMemoryHandleTypeOpaqueWin32Kmt;
    md.handle.win32.handle = h;
#else
    md.type = hipExternalMemoryHandleTypeOpaqueFd;
    md.handle.fd = h;
#endif
    hipExternalMemory_t mem; CHIP(hipImportExternalMemory(&mem, &md), __LINE__);
    hipExternalMemoryBufferDesc bd{0, (unsigned long long)sz, 0};
    CHIP(hipExternalMemoryGetMappedBuffer(ptr, mem, &bd), __LINE__);
    return true;
}
// HIP kernels
__global__ void InitKernel(Particle* p, uint32_t n, uint64_t seed) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; if (i >= n) return;
    ZC_Rng r; ZC_RngInit(seed, i, 0, &r);
    p[i] = {0,0,0, {0,0,0}, E0, 1.0f, 1}; ZC_GetIsotropicDir(&r, p[i].dir);
}

__global__ void CollisionKernel(Particle* in, Particle* out, uint32_t n, uint32_t b, uint64_t seed) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; if (i >= n) return;
    Particle p = in[i]; if (!p.alive) { out[i] = p; return; }
    ZC_Rng r; ZC_RngInit(seed, i, b+1, &r);
    float u = ZC_Uniform(&r); float d = -logf(u) / SIGMA_T;
    p.x += p.dir[0]*d; p.y += p.dir[1]*d; p.z += p.dir[2]*d;
    u = ZC_Uniform(&r);
    if (u < SIGMA_CAP/SIGMA_T) p.alive = 0;
    else { float cc = 2.0f*ZC_Uniform(&r)-1.0f; float er; ZC_ElasticScatter(A_MAT, &r, p.E, p.dir, cc, er); }
    out[i] = p;
}

// Host-side checksum so ref and interop paths share one formula
static Checksum ChecksumHost(const Particle* d_p, uint32_t n, hipStream_t stream = nullptr) {
    Particle* h = (Particle*)malloc(sizeof(Particle) * n);
    if (stream) {
        CHIP(hipMemcpyAsync(h, d_p, sizeof(Particle) * n, hipMemcpyDeviceToHost, stream), __LINE__);
        CHIP(hipStreamSynchronize(stream), __LINE__);
    } else {
        CHIP(hipMemcpy(h, d_p, sizeof(Particle) * n, hipMemcpyDeviceToHost), __LINE__);
    }
    Checksum c{};
    for (uint32_t i = 0; i < n; i++) {
        if (!h[i].alive) continue;
        c.alive_count++;
        if (h[i].E > 0) c.logE_sum += (unsigned long long)((logf(h[i].E)+20.0f)*1e9f + 0.5f);
        c.pos_sum += (unsigned long long)((fabsf(h[i].x)+fabsf(h[i].y)+fabsf(h[i].z))*1e9f + 0.5f);
    }
    free(h);
    return c;
}
// Run reference mode - returns checksum
static Checksum RunRef(uint32_t N, uint32_t bounces, uint64_t seed) {
    size_t ps = sizeof(Particle) * N;
    Particle *d_a, *d_b;
    CHIP(hipMalloc(&d_a, ps), __LINE__);
    CHIP(hipMalloc(&d_b, ps), __LINE__);
    const uint32_t bs = 256, gs = (N + bs - 1) / bs;
    // Init writes B (d_b) for first bounce to read
    hipLaunchKernelGGL(InitKernel, gs, dim3(bs), 0, g_stream, d_b, N, seed);
    CHIP(hipGetLastError(), __LINE__);
    CHIP(hipStreamSynchronize(g_stream), __LINE__);
    for (uint32_t b = 0; b < bounces; b++) {
        hipLaunchKernelGGL(CollisionKernel, gs, dim3(bs), 0, g_stream, d_b, d_a, N, b, seed);
        CHIP(hipGetLastError(), __LINE__);
        CHIP(hipMemcpyAsync(d_b, d_a, ps, hipMemcpyDeviceToDevice, g_stream), __LINE__);
    }
    CHIP(hipStreamSynchronize(g_stream), __LINE__);
    Checksum c = ChecksumHost(d_b, N, g_stream);
    CHIP(hipFree(d_a), __LINE__);
    CHIP(hipFree(d_b), __LINE__);
    return c;
}

static const char* HipErrorName(hipError_t e) {
    const char* n = hipGetErrorName(e);
    return n ? n : "unknown";
}

enum class SyncMode { None, Hip, Vk, All };
enum class Pattern { Batch, Interleave };
struct Options {
    uint32_t N = 10000, bounces = 100, runs = 50, timeout_ms = 5000;
    SyncMode sync = SyncMode::None;
    Pattern pattern = Pattern::Batch;
    bool patternGiven = false;
    bool qfot = false; // false: global memory barriers (default); true: queue-family ownership transfer
    bool hostFenceWait = false; // interleave end-wait: false = HIP stream only; true = vkWaitForFences first
};

static Options ParseArgs(int argc, char** argv, const char* exeName) {
    Options o;
    auto usage = [&]() {
        fprintf(stderr, "Usage: %s [--particles N] [--bounces N] [--runs N] [--timeout-ms N] [--pattern batch|interleave] [--sync none|hip|vk|all] [--qfot] [--no-ext-barriers] [--host-fence-wait]\n", exeName);
        exit(1);
    };
    for (int i = 1; i < argc; i++) {
        const bool hasVal = i + 1 < argc;
        if (!strcmp(argv[i], "--particles") && hasVal) o.N = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bounces") && hasVal) o.bounces = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--runs") && hasVal) o.runs = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout-ms") && hasVal) o.timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sync") && hasVal) {
            const char* s = argv[++i];
            if (!strcmp(s, "none")) o.sync = SyncMode::None;
            else if (!strcmp(s, "hip")) o.sync = SyncMode::Hip;
            else if (!strcmp(s, "vk")) o.sync = SyncMode::Vk;
            else if (!strcmp(s, "all")) o.sync = SyncMode::All;
            else usage();
        }
        else if (!strcmp(argv[i], "--pattern") && hasVal) {
            const char* s = argv[++i];
            if (!strcmp(s, "batch")) o.pattern = Pattern::Batch;
            else if (!strcmp(s, "interleave")) o.pattern = Pattern::Interleave;
            else usage();
            o.patternGiven = true;
        }
        else if (!strcmp(argv[i], "--qfot")) o.qfot = true;
        else if (!strcmp(argv[i], "--host-fence-wait")) o.hostFenceWait = true;
        else if (!strcmp(argv[i], "--no-ext-barriers")) o.qfot = false; // no-op alias for the default
        else usage();
    }
    if (!o.patternGiven && o.sync != SyncMode::None) o.pattern = Pattern::Interleave;
    if (o.patternGiven && o.pattern == Pattern::Batch && o.sync != SyncMode::None) {
        fprintf(stderr, "--pattern batch cannot be combined with --sync hip|vk|all: per-bounce CPU sync is impossible when the whole run is pre-enqueued\n");
        exit(2);
    }
    return o;
}

static void InitAll(bool needD3D12) {
    // HIP
    CHIP(hipInit(0), __LINE__);
    int dc; CHIP(hipGetDeviceCount(&dc), __LINE__); if (!dc) { fprintf(stderr, "No HIP devices\n"); exit(1); }
    hipDeviceProp_t dp; CHIP(hipGetDeviceProperties(&dp, 0), __LINE__);
    CHIP(hipSetDevice(0), __LINE__);
    CHIP(hipStreamCreateWithFlags(&g_stream, hipStreamNonBlocking), __LINE__);

    // Vulkan
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "InteropRepro", 0, "NoEngine", 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &ai, 0, nullptr, 0, nullptr};
    CVK(vkCreateInstance(&ic, nullptr, &g_vkInst), __LINE__);
    uint32_t pc; CVK(vkEnumeratePhysicalDevices(g_vkInst, &pc, nullptr), __LINE__);
    VkPhysicalDevice* pds = new VkPhysicalDevice[pc]; CVK(vkEnumeratePhysicalDevices(g_vkInst, &pc, pds), __LINE__);
    g_vkPhys = pds[0]; delete[] pds;
    VkPhysicalDeviceProperties pdp; vkGetPhysicalDeviceProperties(g_vkPhys, &pdp);

    int rtv = 0, drv = 0; hipRuntimeGetVersion(&rtv); hipDriverGetVersion(&drv);
    printf("HIP device: %s (%s)\n", dp.name, dp.gcnArchName);
    printf("HIP runtime version: %d, driver version: %d\n", rtv, drv);
    printf("timelineSemaphoreInteropSupported: %d\n", (int)dp.timelineSemaphoreInteropSupported);
    printf("Vulkan apiVersion: %u.%u.%u\n", VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion), VK_VERSION_PATCH(pdp.apiVersion));
    printf("Vulkan driverVersion: 0x%x (%u)\n", pdp.driverVersion, pdp.driverVersion);
    printf("Vulkan vendorID: 0x%04x deviceID: 0x%04x\n", pdp.vendorID, pdp.deviceID);
    printf("Vulkan device: %s\n", pdp.deviceName);

    uint32_t qfc; vkGetPhysicalDeviceQueueFamilyProperties(g_vkPhys, &qfc, nullptr);
    VkQueueFamilyProperties* qfp = new VkQueueFamilyProperties[qfc];
    vkGetPhysicalDeviceQueueFamilyProperties(g_vkPhys, &qfc, qfp);
    for (uint32_t i = 0; i < qfc; i++) if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vkQF = i; break; }
    delete[] qfp;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, g_vkQF, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
#ifdef _WIN32
    const char* devExts[] = {"VK_KHR_external_memory", "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore", "VK_KHR_external_semaphore_win32"};
#else
    const char* devExts[] = {"VK_KHR_external_memory", "VK_KHR_external_memory_fd",
        "VK_KHR_external_semaphore", "VK_KHR_external_semaphore_fd"};
#endif
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, nullptr, 4, devExts};
    CVK(vkCreateDevice(g_vkPhys, &dci, nullptr, &g_vkDev), __LINE__);
    vkGetDeviceQueue(g_vkDev, g_vkQF, 0, &g_vkQueue);

#ifdef _WIN32
    if (needD3D12) {
        IDXGIAdapter1* da = nullptr;
        if (!LUIDMatch(g_vkPhys, &da, &g_d3d12Dev)) { fprintf(stderr, "LUID match failed\n"); exit(1); }
    }
#else
    (void)needD3D12;
#endif
}

static void SetupBuffers(const Options& opt) {
    size_t ps = sizeof(Particle) * opt.N;
    if (!CreateExpBuf(ps, &g_bufA, &g_memA, &g_kmtA) || !CreateExpBuf(ps, &g_bufB, &g_memB, &g_kmtB)) { fprintf(stderr, "Buffer creation failed\n"); exit(1); }
    if (!ImportBuf(g_kmtA, ps, &g_d_bufA) || !ImportBuf(g_kmtB, ps, &g_d_bufB)) { fprintf(stderr, "Buffer import failed\n"); exit(1); }
    // Record copy command buffer once
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, g_vkQF, 0};
    CVK(vkCreateCommandPool(g_vkDev, &pci, nullptr, &g_cmdPool), __LINE__);
    VkCommandBufferAllocateInfo aci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    CVK(vkAllocateCommandBuffers(g_vkDev, &aci, &g_cmdBuf), __LINE__);
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    CVK(vkBeginCommandBuffer(g_cmdBuf, &bbi), __LINE__);
    VkBufferCopy rc{0, 0, ps};
    if (opt.qfot) {
        // Queue-family ownership transfer: acquire from / release to the external (HIP) user
        VkBufferMemoryBarrier acq[2] = {
            {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_QUEUE_FAMILY_EXTERNAL, g_vkQF, g_bufA, 0, VK_WHOLE_SIZE},
            {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_QUEUE_FAMILY_EXTERNAL, g_vkQF, g_bufB, 0, VK_WHOLE_SIZE}};
        vkCmdPipelineBarrier(g_cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 2, acq, 0, nullptr);
        vkCmdCopyBuffer(g_cmdBuf, g_bufA, g_bufB, 1, &rc);
        VkBufferMemoryBarrier rel[2] = {
            {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_READ_BIT, 0,
                g_vkQF, VK_QUEUE_FAMILY_EXTERNAL, g_bufA, 0, VK_WHOLE_SIZE},
            {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                g_vkQF, VK_QUEUE_FAMILY_EXTERNAL, g_bufB, 0, VK_WHOLE_SIZE}};
        vkCmdPipelineBarrier(g_cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 2, rel, 0, nullptr);
    } else {
        VkMemoryBarrier preBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(g_cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &preBarrier, 0, nullptr, 0, nullptr);
        vkCmdCopyBuffer(g_cmdBuf, g_bufA, g_bufB, 1, &rc);
        VkMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(g_cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &postBarrier, 0, nullptr, 0, nullptr);
    }
    CVK(vkEndCommandBuffer(g_cmdBuf), __LINE__);
    // Create fence for hang detection
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    CVK(vkCreateFence(g_vkDev, &fci, nullptr, &g_vkFence), __LINE__);
}

// Requires SetupBuffers and the four semaphore globals (g_vkHipDone, g_vkVkDone, g_hipHipDone, g_hipVkDone) to be set up.
static int RunPingPong(const Options& opt, bool d3d12Submit) {
#ifndef _WIN32
    d3d12Submit = false; // no D3D12 on Linux
#endif
    const uint32_t N = opt.N, bounces = opt.bounces, runs = opt.runs, timeout_ms = opt.timeout_ms;
    const char* syncNames[] = {"none", "hip", "vk", "all"};

    // Golden checksum from a plain HIP run
    Checksum golden = RunRef(N, bounces, 42);
    Checksum golden_prev = (bounces > 0) ? RunRef(N, bounces - 1, 42) : Checksum{};
    printf("Golden: a=%llu lE=%llu p=%llu\n", golden.alive_count, golden.logE_sum, golden.pos_sum);
    if (bounces > 0) {
        printf("Golden-1: a=%llu lE=%llu p=%llu\n", golden_prev.alive_count, golden_prev.logE_sum, golden_prev.pos_sum);
    }
    printf("config: particles=%u bounces=%u runs=%u pattern=%s sync=%s barriers=%s endwait=%s\n",
        N, bounces, runs, opt.pattern == Pattern::Batch ? "batch" : "interleave",
        syncNames[(int)opt.sync], opt.qfot ? "qfot" : "global", opt.hostFenceWait ? "host-fence" : "hip-only");

    const uint32_t bs = 256, gs = (N + bs - 1) / bs;
    uint64_t seed = 42; unsigned long long fenceBase = 0;
    uint32_t pass = 0, mismatch = 0;
    for (uint32_t r = 0; r < runs; r++) {
        // Particle re-init before each run - writes B
        hipLaunchKernelGGL(InitKernel, gs, dim3(bs), 0, g_stream, (Particle*)g_d_bufB, N, seed);
        CHIP(hipGetLastError(), __LINE__);
        CHIP(hipStreamSynchronize(g_stream), __LINE__);
        auto start = std::chrono::high_resolution_clock::now();

        if (opt.pattern == Pattern::Interleave) {
            CVK(vkResetFences(g_vkDev, 1, &g_vkFence), __LINE__);
            for (uint32_t b = 0; b < bounces; b++) {
                unsigned long long val = fenceBase + b + 1;
                // HIP: kernel(read B, write A)
                hipLaunchKernelGGL(CollisionKernel, gs, dim3(bs), 0, g_stream,
                    (Particle*)g_d_bufB, (Particle*)g_d_bufA, N, b, seed);
                CHIP(hipGetLastError(), __LINE__);
                // signal hipDone with value val
                hipExternalSemaphoreSignalParams sp{}; sp.params.fence.value = val;
                CHIP(hipSignalExternalSemaphoresAsync(&g_hipHipDone, &sp, 1, g_stream), __LINE__);
                // Vulkan: wait hipDone val, copy A->B, signal vkDone val
                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr,
                    1, &g_vkHipDone, &waitStage, 1, &g_cmdBuf, 1, &g_vkVkDone};
                // both structs live until vkQueueSubmit (no dangling pNext)
                VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,
                    1, &val, 1, &val};
#ifdef _WIN32
                VkD3D12FenceSubmitInfoKHR fi{VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR, nullptr,
                    1, &val, 1, &val};
                // d3d12: timeline sems need VkTimelineSemaphoreSubmitInfo too
                fi.pNext = &ti;
                si.pNext = d3d12Submit ? (const void*)&fi : (const void*)&ti;
#else
                si.pNext = &ti;
#endif
                VkFence submitFence = (b == bounces - 1) ? g_vkFence : VK_NULL_HANDLE;
                CVK(vkQueueSubmit(g_vkQueue, 1, &si, submitFence), __LINE__);
                // HIP: wait vkDone val
                hipExternalSemaphoreWaitParams wp{}; wp.params.fence.value = val;
                CHIP(hipWaitExternalSemaphoresAsync(&g_hipVkDone, &wp, 1, g_stream), __LINE__);
                // optional sync after every bounce
                if (opt.sync == SyncMode::Hip || opt.sync == SyncMode::All) { CHIP(hipStreamSynchronize(g_stream), __LINE__); }
                if (opt.sync == SyncMode::Vk || opt.sync == SyncMode::All) { CVK(vkQueueWaitIdle(g_vkQueue), __LINE__); }
            }
            // Hang detection: optionally wait for the last submit's fence (--host-fence-wait), then the HIP stream (as in batch)
            auto deadline = start + std::chrono::milliseconds(timeout_ms);
            while (opt.hostFenceWait) {
                VkResult fr = vkWaitForFences(g_vkDev, 1, &g_vkFence, VK_FALSE, 100000);
                if (fr == VK_SUCCESS) break;
                if (fr != VK_TIMEOUT) { fprintf(stderr, "vkWaitForFences: %d\n", fr); return 1; }
                if (std::chrono::high_resolution_clock::now() > deadline) {
                    printf("HANG at run %u\n", r); fflush(stdout); _exit(3);
                }
            }
            while (true) {
                hipError_t se = hipStreamQuery(g_stream);
                if (se == hipSuccess) break;
                if (se != hipErrorNotReady) { fprintf(stderr, "hipStreamQuery: %s\n", HipErrorName(se)); return 2; }
                SleepMs(1);
                if (std::chrono::high_resolution_clock::now() > deadline) {
                    printf("HANG at run %u\n", r); fflush(stdout); _exit(3);
                }
            }
        } else {
            // Batch: pattern derived from a production app - whole run enqueued on g_stream, then one vkQueueSubmit
            for (uint32_t b = 0; b < bounces; b++) {
                unsigned long long val = fenceBase + b + 1;
                hipLaunchKernelGGL(CollisionKernel, gs, dim3(bs), 0, g_stream,
                    (Particle*)g_d_bufB, (Particle*)g_d_bufA, N, b, seed);
                CHIP(hipGetLastError(), __LINE__);
                hipExternalSemaphoreSignalParams sp{}; sp.params.fence.value = val;
                CHIP(hipSignalExternalSemaphoresAsync(&g_hipHipDone, &sp, 1, g_stream), __LINE__);
                hipExternalSemaphoreWaitParams wp{}; wp.params.fence.value = val;
                CHIP(hipWaitExternalSemaphoresAsync(&g_hipVkDone, &wp, 1, g_stream), __LINE__);
            }
            // Vulkan: one submit with a VkSubmitInfo per bounce; vectors must outlive the submit
            std::vector<unsigned long long> vals(bounces);
            std::vector<VkSubmitInfo> submits(bounces);
#ifdef _WIN32
            std::vector<VkD3D12FenceSubmitInfoKHR> fis(bounces);
#endif
            std::vector<VkTimelineSemaphoreSubmitInfo> tis(bounces);
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            for (uint32_t b = 0; b < bounces; b++) {
                vals[b] = fenceBase + b + 1;
                const void* pn;
#ifdef _WIN32
                if (d3d12Submit) {
                    // D3D12 fence submit info only (no VkTimelineSemaphoreSubmitInfo), as the production app does
                    fis[b] = VkD3D12FenceSubmitInfoKHR{VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR, nullptr,
                        1, &vals[b], 1, &vals[b]};
                    pn = &fis[b];
                } else
#endif
                {
                    tis[b] = VkTimelineSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,
                        1, &vals[b], 1, &vals[b]};
                    pn = &tis[b];
                }
                submits[b] = VkSubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO, pn,
                    1, &g_vkHipDone, &waitStage, 1, &g_cmdBuf, 1, &g_vkVkDone};
            }
            CVK(vkResetFences(g_vkDev, 1, &g_vkFence), __LINE__);
            CVK(vkQueueSubmit(g_vkQueue, bounces, submits.data(), g_vkFence), __LINE__);
            // Host waits: fence in 100 ms slices, then poll the stream
            auto deadline = start + std::chrono::milliseconds(timeout_ms);
            while (true) {
                VkResult fr = vkWaitForFences(g_vkDev, 1, &g_vkFence, VK_FALSE, 100000);
                if (fr == VK_SUCCESS) break;
                if (fr != VK_TIMEOUT) { fprintf(stderr, "vkWaitForFences: %d\n", fr); return 1; }
                if (std::chrono::high_resolution_clock::now() > deadline) {
                    printf("HANG at run %u\n", r); fflush(stdout); _exit(3);
                }
            }
            while (true) {
                hipError_t se = hipStreamQuery(g_stream);
                if (se == hipSuccess) break;
                if (se != hipErrorNotReady) { fprintf(stderr, "hipStreamQuery: %s\n", HipErrorName(se)); return 2; }
                SleepMs(1);
                if (std::chrono::high_resolution_clock::now() > deadline) {
                    printf("HANG at run %u\n", r); fflush(stdout); _exit(3);
                }
            }
        }
        CHIP(hipStreamSynchronize(g_stream), __LINE__); // loop may exit on the VK fence before the HIP wait drains
        // diag: semaphore counter and fence status
        {
            uint64_t cv = 0;
            vkGetSemaphoreCounterValue(g_vkDev, g_vkVkDone, &cv);
            VkResult fr = vkGetFenceStatus(g_vkDev, g_vkFence);
            printf("diag run=%u cv=%llu target=%llu fence=%d\n", r, cv, fenceBase+bounces, fr);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / (double)bounces;

        Particle* fb = (Particle*)g_d_bufB;
        CHIP(hipDeviceSynchronize(), __LINE__);
        Checksum c = ChecksumHost(fb, N, g_stream);

        bool m = (c.alive_count == golden.alive_count && c.logE_sum == golden.logE_sum && c.pos_sum == golden.pos_sum);
        bool stale_by_one = (bounces > 0) &&
            (c.alive_count == golden_prev.alive_count && c.logE_sum == golden_prev.logE_sum && c.pos_sum == golden_prev.pos_sum);

        if (!m) {
            // Recheck after sync and sleep
            CHIP(hipDeviceSynchronize(), __LINE__);
            SleepMs(10);
            Checksum c_recheck = ChecksumHost(fb, N, g_stream);
            bool recheck_match = (c_recheck.alive_count == golden.alive_count &&
                c_recheck.logE_sum == golden.logE_sum && c_recheck.pos_sum == golden.pos_sum);
            printf("run %u: a=%llu lE=%llu p=%llu match=no stale_by_one=%s recheck=%s us/b=%.2f\n",
                r, c.alive_count, c.logE_sum, c.pos_sum,
                stale_by_one ? "yes" : "no", recheck_match ? "yes" : "no", us);
        } else {
            printf("run %u: a=%llu lE=%llu p=%llu match=yes us/b=%.2f\n", r, c.alive_count, c.logE_sum, c.pos_sum, us);
        }
        if (m) pass++; else mismatch++;
        fenceBase += bounces;
    }

    if (!mismatch) { printf("PASS %u/%u\n", pass, runs); return 0; }
    else { printf("MISMATCH %u/%u\n", mismatch, runs); return 1; }
}
