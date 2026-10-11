#!/usr/bin/env python3

from test_native_flac_external_decode import select_backend_device_from_devices_output


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def require_selection(devices_output, backend, requested_device, expected_device):
    selected, skip_reason = select_backend_device_from_devices_output(
        devices_output,
        backend,
        requested_device,
    )
    require(skip_reason is None, f"unexpected skip: {skip_reason}")
    require(selected == expected_device, f"expected {expected_device}, got {selected}")


def require_skip(devices_output, backend, requested_device, expected_text):
    selected, skip_reason = select_backend_device_from_devices_output(
        devices_output,
        backend,
        requested_device,
    )
    require(selected is None, f"unexpected selected device: {selected}")
    require(skip_reason is not None, "expected a skip reason")
    require(
        expected_text in skip_reason,
        f"expected skip text {expected_text!r}, got {skip_reason!r}",
    )


MIXED_DEVICES = """\
OpenCL support: built
[0] NVIDIA GeForce RTX 4070 SUPER
    available: yes
[1] NVIDIA GeForce RTX 5070 Ti
    available: yes

Vulkan support: built
[0] AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)
    type: integrated-gpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes
[1] NVIDIA GeForce RTX 5070 Ti
    type: discrete-gpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes
[2] NVIDIA GeForce RTX 4070 SUPER
    type: discrete-gpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes
[3] llvmpipe (LLVM 19.1.7, 256 bits)
    type: cpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes

Metal support: built
[0] Apple M5 Pro
    available: yes
    low power: no
[1] Low Power GPU
    available: yes
    low power: yes
"""

INTEGRATED_AND_CPU_DEVICES = """\
OpenCL support: built
No OpenCL devices found

Vulkan support: built
[0] AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)
    type: integrated-gpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes
[1] llvmpipe (LLVM 19.1.7, 256 bits)
    type: cpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes

Metal support: built
[0] Integrated Apple GPU
    available: yes
    low power: yes
"""

CPU_ONLY_DEVICES = """\
OpenCL support: built
No OpenCL devices found

Vulkan support: built
[0] llvmpipe (LLVM 19.1.7, 256 bits)
    type: cpu
    available: yes
    shaderInt64: yes
    vulkan backend usable: yes

Metal support: built
No Metal devices found
"""

UNUSABLE_REQUESTED_DEVICES = """\
OpenCL support: built
No OpenCL devices found

Vulkan support: built
[0] AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)
    type: integrated-gpu
    available: yes
    shaderInt64: no
    vulkan backend usable: no

Metal support: built
[0] Offline Metal Device
    available: no
    low power: no
"""


def main():
    require_selection(MIXED_DEVICES, "opencl", None, "0")
    require_selection(MIXED_DEVICES, "opencl", "1", "1")
    require_skip(MIXED_DEVICES, "opencl", "9", "not visible")

    require_selection(MIXED_DEVICES, "vulkan", None, "1")
    require_selection(MIXED_DEVICES, "vulkan", "0", "0")
    require_selection(INTEGRATED_AND_CPU_DEVICES, "vulkan", None, "0")
    require_skip(CPU_ONLY_DEVICES, "vulkan", None, "non-CPU")
    require_selection(CPU_ONLY_DEVICES, "vulkan", "0", "0")
    require_skip(UNUSABLE_REQUESTED_DEVICES, "vulkan", "0", "not backend-usable")
    require_skip(MIXED_DEVICES, "vulkan", "9", "not visible")

    require_selection(MIXED_DEVICES, "metal", None, "0")
    require_selection(MIXED_DEVICES, "metal", "1", "1")
    require_selection(INTEGRATED_AND_CPU_DEVICES, "metal", None, "0")
    require_skip(CPU_ONLY_DEVICES, "metal", None, "no available")
    require_skip(UNUSABLE_REQUESTED_DEVICES, "metal", "0", "not available")
    require_skip(MIXED_DEVICES, "metal", "9", "not visible")


if __name__ == "__main__":
    main()
