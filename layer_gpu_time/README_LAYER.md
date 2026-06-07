# Layer: GPU Time

This layer is a frame timing profiler that captures precise, non-overlapping
GPU execution times for individual Vulkan workloads (render passes, compute
dispatches, transfers, and ray tracing operations) for selected frames of interest.

## What devices are supported?

This layer is cross-platform and supports any Vulkan 1.0+ compatible GPU on
Android and Linux.

It takes advantage of modern Vulkan features when available, automatically
utilizing Vulkan 1.3 Synchronization 2 (`VK_KHR_synchronization2`) and CPU-side
query resetting (`VK_EXT_host_query_reset`). If these features are not
supported or enabled by the host application, the layer automatically falls
back to standard Vulkan 1.0 execution barriers and pipeline stage timestamps.

## What data is collected?

The layer serializes GPU execution by inserting full memory and execution
barriers immediately before and after every recorded Vulkan workload.

By injecting GPU timestamp queries around each isolated operation, the layer
captures the absolute, non-overlapping active GPU duration of each workload,
completely eliminating pipelined execution noise.

The final output collects the elapsed execution time for:
*   **Render Passes**: Direct and dynamic rendering passes.
*   **Compute**: Compute dispatches (`compute`).
*   **Ray Tracing**: Ray tracing dispatches and acceleration structure builds (`tracerays`, `as_build`).
*   **Transfers**: Memory copies, clears, and fills (`buffer_transfer`, `image_transfer`).

The execution times are calculated in milliseconds using the physical device's
reported hardware timestamp period.

## How do I use the layer?

### Prerequisites

Device setup steps:

*   Ensure your Android device is in developer mode, with `adb` support enabled.
*   Ensure the Android device is connected to your development workstation with an authorized debug connection.

Application setup steps:

*   Build a debuggable build of your application and install it on the Android device.

Tooling setup steps:

*   Install the Android platform tools and ensure `adb` is on your `PATH`.
*   Install the Android NDK and set the `ANDROID_NDK_HOME` environment variable to its installation path.

### Layer build

Build the Time layer for Android using the provided build script from the `layer_gpu_time` directory:

```sh
# Refer to the general build instructions in the docs folder for building on Android or Linux.
```

### Running using the layer

Configure the target device by enabling the layer and supplying the configuration JSON file using the Android helper utility found in the root directory:

```sh
python3 lgl_android_install.py --layer layer_gpu_time --config <your_config.json> --profile <out_dir>
```

The [`layer_config.json`](layer_config.json) file in this directory is a template configuration file you can use as a starting point.

The output will write individual JSON/CSV data files to the host workstation containing the `Time` duration in milliseconds for every Vulkan workload profiled in the selected frames.

## Layer configuration

### Setting frame selection mode

The layer supports the following frame selection modes configured via the `frame_mode` config option:

*   `disabled`: Profile sampling is fully disabled.
*   `periodic`: Profiles frames periodically. The integer value of `periodic_frame` defines the frequency (e.g., sample every `10` frames), and `periodic_min_frame` defines the first frame at which to begin sampling.
*   `list`: Profiles specific, discrete frames. The value of `frame_list` defines an array of integers representing the specific frame IDs to capture (e.g., `[5, 8, 13, 21]`).
