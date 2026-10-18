# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: `#include <mutex>` in the FFX Vulkan backend; libc++ (the Windows MSYS2 CLANG64
  build) does not include it transitively.
- `0003-...`: the 3.1.6 frame-generation wrapper tags the bridge frame on Prepare and
  retires its transient descriptor sets and constant buffers in RetireFrame, like the
  3.1.5 upscaler wrapper (without it every dispatch leaked them until OutOfDeviceMemory).
- `0004-...`: the 3.1.5 bridge (used by 3.1.6 frame generation) stages its per-pass constant
  buffers into persistent 256 KiB mapped slabs instead of one `vkCreateBuffer` +
  `vkAllocateMemory` each (dozens per frame, freed a few frames later).
- `0005-...`: the FFX 1.1.4 Vulkan backend (the FSR 3 upscaler) falls back to plain host memory
  for its constant buffers when the device-local host-visible heap is full. Without Resizable
  BAR that heap is ~214 MB and the game fills it: the upscaler's context creation then failed
  with "out of memory" while VRAM was mostly free (measured on an RTX 3070, 2026-10-18).
