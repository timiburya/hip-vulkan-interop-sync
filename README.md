# hip-vulkan-interop-sync

A minimal test harness for HIP ↔ Vulkan interop on AMD GPUs. HIP and Vulkan share external memory and exchange timeline semaphores (D3D12 fence or OpaqueWin32 on Windows, OpaqueFd on Linux). A stress matrix checks whether the cross-API ordering holds. All results below are from Windows. The Linux build is provided for convenience and has not been tested.

Context: [ROCm/hip#3909](https://github.com/ROCm/hip/issues/3909).

## Summary of the observation

- **What happens.** A HIP stream ends with `hipWaitExternalSemaphoresAsync(vkDone = N)`. In rare runs, a device-to-host copy enqueued on the same stream after that wait reads data from *before* the Vulkan work that signals `N`. This happens even after `hipStreamSynchronize` and `hipDeviceSynchronize` have returned.
- **What the reader sees.** In most such runs the data is "stale by one": the last Vulkan copy is missing. Re-reading 10 ms later gives the correct data.
- **Where it shows up.** Only with interleaved per-bounce submission, about 0.04–0.6 % of runs. Batch submission was clean in every run.
- **What does not help.** Calling `hipStreamSynchronize` after every bounce does not help. That cell is the worst one.
- **What hides it.** A host-side Vulkan wait (`vkQueueWaitIdle` per bounce, or `vkWaitForFences` on the last submit before trusting the HIP stream) hides it completely.
- **Vulkan side.** The Vulkan barriers are recorded in every configuration: a global `VkMemoryBarrier`, or a queue-family ownership transfer with `--qfot`.

## The test

Each run does the following:

1. A HIP kernel initialises particle buffer **B**.
2. The run does `--bounces` (default 100) iterations of:
   - HIP `CollisionKernel` reads **B** and writes **A**.
   - HIP `hipSignalExternalSemaphoresAsync(hipDone = v)`.
   - Vulkan `vkQueueSubmit`: wait `hipDone = v`, `vkCmdCopyBuffer A → B` with barriers, signal `vkDone = v`.
   - HIP `hipWaitExternalSemaphoresAsync(vkDone = v)`.
3. The host waits (see "end-wait" below).
4. **B** is copied to the host with `hipMemcpyAsync` on the same stream and checksummed. It is compared with a HIP-only golden run, and with the golden run of `bounces − 1` (the `stale_by_one` check).

Each executable runs `--runs` (default 50) runs per invocation. The semaphore values keep increasing across runs.

Programs:

| executable | semaphore type (Vulkan export → HIP import) |
|---|---|
| `repro_d3d12_fence` | `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT` → `hipExternalSemaphoreHandleTypeD3D12Fence` |
| `repro_opaque_win32` | Vulkan timeline semaphore, `OPAQUE_WIN32` → `hipExternalSemaphoreHandleTypeOpaqueWin32` |
| `repro_opaque_fd` (Linux, untested) | Vulkan timeline semaphore, `OPAQUE_FD` → `hipExternalSemaphoreHandleTypeOpaqueFd` |

`repro_opaque_win32` and `repro_opaque_fd` are built from the same source, `src/repro_opaque.hip`. On Windows the memory is a Vulkan buffer exported as `OPAQUE_WIN32_KMT` and imported with `hipImportExternalMemory` (`hipExternalMemoryHandleTypeOpaqueWin32Kmt`). On Linux it is exported as `OPAQUE_FD` and imported as `hipExternalMemoryHandleTypeOpaqueFd`.

Options:

| option | meaning |
|---|---|
| `--pattern batch` | Enqueue the whole run on the HIP stream first, then one `vkQueueSubmit` with one `VkSubmitInfo` per bounce. The host waits on the Vulkan fence, then on the stream. |
| `--pattern interleave` | One HIP kernel, one `vkQueueSubmit` and one HIP wait per bounce. |
| `--sync none\|hip\|vk\|all` | Interleave only: a host sync after every bounce. `hip` = `hipStreamSynchronize`, `vk` = `vkQueueWaitIdle`, `all` = both. These are *host syncs*, not barriers. |
| `--qfot` | Use queue-family ownership transfers to and from `VK_QUEUE_FAMILY_EXTERNAL` instead of global memory barriers. |
| `--host-fence-wait` | Interleave end-wait: `vkWaitForFences` on the last submit, *then* poll the HIP stream. Without it (the default), the host trusts only the HIP stream (`hipStreamQuery` until success). |
| `--particles N --bounces N --runs N --timeout-ms N` | Problem size, bounces per run, runs per invocation, hang timeout. |

Exit codes: 0 = all runs matched, 1 = mismatch, 2 = import or API failure, 3 = hang (timeout).

## Build

The GPU architecture is detected automatically with `amdgpu-arch`, `rocm_agent_enumerator` or `hipInfo` from the ROCm install. To set it yourself, pass `-DCMAKE_HIP_ARCHITECTURES=gfxXXXX`.

### Windows

You need:

- the AMD HIP SDK (`HIP_PATH` set);
- the Vulkan SDK (`VULKAN_SDK` set);
- Visual Studio 2022 with the C++ workload, which provides the STL and Windows SDK headers (the compiler is ROCm clang);
- CMake ≥ 3.21 and Ninja on `PATH`.

```bat
scripts\build.cmd
```

The script finds Visual Studio, then configures and builds into `build\` with Ninja. Extra arguments go to CMake, for example `scripts\build.cmd -DCMAKE_HIP_ARCHITECTURES=gfx1100`. It builds `repro_d3d12_fence.exe` and `repro_opaque_win32.exe`.

The executables load `amdhip64` from the HIP SDK. Put `%HIP_PATH%\bin` on `PATH` before you run them.

### Linux (untested)

You need ROCm (default `/opt/rocm`; override with `ROCM_PATH` or `-DROCM_PATH=…`), the Vulkan headers and loader (for example `libvulkan-dev`, or the LunarG SDK), and CMake ≥ 3.21.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds `repro_opaque_fd` only. There is no D3D12 variant on Linux. The Linux code path compiles against the ROCm and Vulkan headers but has not been run, so please report any problems.

## Run

Windows:

```bat
set PATH=%HIP_PATH%\bin;%PATH%
build\repro_d3d12_fence.exe  --pattern interleave --sync hip
build\repro_opaque_win32.exe --pattern interleave --sync hip
build\repro_d3d12_fence.exe  --pattern batch
```

Linux:

```sh
build/repro_opaque_fd --pattern interleave --sync hip
```

A single invocation (50 runs) usually passes, because the failure rate is low. Use the matrix script instead. It runs many invocations and writes `summary.md`.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run_matrix.ps1 -Invocations 50 `
    -Combos batch-none,inter-none,inter-hip,inter-vk
powershell -ExecutionPolicy Bypass -File scripts\run_matrix.ps1 -Invocations 20 `
    -Combos inter-none -ExtraArgs '--host-fence-wait' -OutDir results\hostfence
```

```sh
scripts/run_matrix.sh -n 50 -c batch-none,inter-none,inter-hip,inter-vk -o results/linux
scripts/run_matrix.sh -n 20 -c inter-none -x --host-fence-wait -o results/linux-hostfence
```

Mismatching runs print `match=no stale_by_one=yes|no recheck=yes|no`. After every run, a `diag` line prints the Vulkan semaphore counter and the fence status as seen after the HIP syncs.

## Results

Environment:

- AMD Radeon RX 7600S (gfx1102), Windows 11;
- HIP runtime 70260201 (HIP SDK 7.2.6);
- Vulkan 1.4.315, driver version 0x800161;
- `timelineSemaphoreInteropSupported = 0`.

Each cell is 50 invocations × 50 runs = 2500 runs. Full tables and every mismatch line are in [`results/`](results/).

**HIP-only end-wait** (the default; [`results/03_hip_only_endwait`](results/03_hip_only_endwait/)):

| combo | d3d12 fence: bad / stale-by-one | opaque win32: bad / stale-by-one |
|---|---|---|
| batch-none | 0 / 0 | 0 / 0 |
| inter-none | 1 / 0 | 1 / 1 |
| inter-hip (`hipStreamSynchronize` per bounce) | **10 / 6** | **16 / 7** |
| inter-vk (`vkQueueWaitIdle` per bounce) | 0 / 0 | 0 / 0 |

**With `--host-fence-wait`**: inter-none was 0 / 1000 on both programs ([`results/04_hip_only_vs_host_fence`](results/04_hip_only_vs_host_fence/)). [`results/02_host_fence_endwait`](results/02_host_fence_endwait/) shows inter-none and inter-hip at 0 / 2500 each on both programs.

[`results/01_pattern_hybrid_endwait`](results/01_pattern_hybrid_endwait/) is an earlier full matrix that also covers `inter-all`. Its end-wait stopped when *either* `hipStreamQuery` succeeded *or* the Vulkan fence was signalled. In all 13 bad runs there, the fence was still unsignalled when HIP reported the stream complete (`diag … fence=1`, counter = target − 1).

What the matrix shows:

- Bad runs happen only when the host relies on HIP's stream completion while the last Vulkan submit is still in flight.
- In every bad run the readback copy is ordered after `hipWaitExternalSemaphoresAsync(vkDone = N)` on the same stream, and the host has called `hipStreamSynchronize` and `hipDeviceSynchronize`. It still reads pre-copy data. The 10 ms recheck is always correct.
- Waits in the middle of a run appear to hold. The failures cluster at the end of the run, where the wait is the last command on the stream.
- Batch submission enqueues all HIP waits before the Vulkan submit, and the host waits on the Vulkan fence first. It has not failed in any run (5000 in this matrix, more earlier).

Caveat: the `diag` status (`fence=1`, counter below target) also shows up often in runs whose data is correct, including after `vkQueueWaitIdle` in the `inter-all` cells. So the status reads themselves can lag. Only the checksum mismatch is hard evidence. We cannot tell from here whether the root cause is on the HIP wait side or in when the Vulkan driver signals the shared semaphore.

## Workarounds that work here

- Batch submission: enqueue all HIP work for a run, then submit the matching Vulkan work in one `vkQueueSubmit`. Wait on the Vulkan fence before trusting the HIP stream.
- In interleaved code, wait on the Vulkan side from the host (`vkWaitForFences` / `vkQueueWaitIdle`) before consuming results on HIP.

## Layout

```
CMakeLists.txt  portable build (Windows: HIP SDK + Vulkan SDK; Linux: ROCm + Vulkan)
src/        repro_common.h (shared harness), repro_d3d12_fence.hip (Windows only), repro_opaque.hip
scripts/    build.cmd + run_matrix.ps1 (Windows), run_matrix.sh (Linux)
results/    summary.md + mismatches.txt per experiment
```
