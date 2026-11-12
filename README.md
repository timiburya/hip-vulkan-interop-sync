# hip-vulkan-interop-sync
 Minimal Windows test harness for HIP ↔ Vulkan GPU interop on AMD: shared external memory plus D3D12-fence and OpaqueWin32 semaphores, with a stress matrix (batch vs interleaved submission, host-sync modes) to detect stale-data synchronization bugs. Context: ROCm/hip#3909.
