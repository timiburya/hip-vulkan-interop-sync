# Results matrix

Environment:

```
HIP device: AMD Radeon RX 7600S (gfx1102)
HIP runtime version: 70260201, driver version: 70260201
timelineSemaphoreInteropSupported: 0
Vulkan apiVersion: 1.4.315
Vulkan driverVersion: 0x800161 (8388961)
Vulkan vendorID: 0x1002 deviceID: 0x7480
Vulkan device: AMD Radeon RX 7600S
D3D12 fence semaphores imported
```

| program | combo | invocations | runs | bad runs | stale_by_one | invocations with mismatch | hangs | import fail |
|---|---|---|---|---|---|---|---|---|
| repro_d3d12_fence | batch-none | 50 | 2500 | 0 | 0 | 0 | 0 | 0 |
| repro_d3d12_fence | inter-none | 50 | 2500 | 1 | 0 | 1 | 0 | 0 |
| repro_d3d12_fence | inter-hip | 50 | 2500 | 10 | 6 | 9 | 0 | 0 |
| repro_d3d12_fence | inter-vk | 50 | 2500 | 0 | 0 | 0 | 0 | 0 |
| repro_opaque_win32 | batch-none | 50 | 2500 | 0 | 0 | 0 | 0 | 0 |
| repro_opaque_win32 | inter-none | 50 | 2500 | 1 | 1 | 1 | 0 | 0 |
| repro_opaque_win32 | inter-hip | 50 | 2500 | 16 | 7 | 10 | 0 | 0 |
| repro_opaque_win32 | inter-vk | 50 | 2500 | 0 | 0 | 0 | 0 | 0 |
