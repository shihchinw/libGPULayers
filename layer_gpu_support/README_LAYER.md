# Layer: GPU Support

This layer is a tech support trick box that is designed to help diagnose
causes of functional and performance issues in applications. It works by
letting you quickly test your application with a set of API behavior overrides
applied, which might help to identify likely problem areas in the application
if an override causes an issue to disappear.

## What devices are supported?

This layer requires Vulkan 1.1 and timeline semaphores, but it does not rely on
any vendor-specific behavior.

## How do I use the layer?

### Prerequisites

Device setup steps:

* Ensure your Android device is in developer mode, with `adb` support enabled
  in developer settings.
* Ensure the Android device is connected to your development workstation, and
  visible to `adb` with an authorized debug connection.

Application setup steps:

* Build a debuggable build of your application and install it on the Android
  device.

Tooling setup steps

* Install the Android platform tools and ensure `adb` is on your `PATH`
  environment variable.
* Install the Android NDK and set the `ANDROID_NDK_HOME` environment variable
  to its installation path.

### Layer build

Build the Support layer for Android using the provided build script, or using
equivalent manual commands, from the `layer_gpu_support` directory. For full
instructions see the _Build an Android layer_ and _Build a Linux layer_
sections in the [Build documentation](../docs/building.md).

### Running using the layer

You configure a device to run support experiments by using the Android helper
utility found in the root directory to configure the layer and manage the
application. You must enable the support layer, and provide a configuration
file to parameterize it.

```sh
python3 lgl_android_install.py --layer layer_gpu_support --config <your.json>
```

The [`layer_config.json`](layer_config.json) file in this directory is a
template configuration file you should start from. It does not enable any
overrides by default, so running the layer using this configuration "as is"
will not do anything useful. Take a copy and modify it to enable the options
you want to try. Details of the configuration options in each override group
are documented in the _Behavior overrides_ section below.

The Android helper utility contains many other options for configuring the
application under test and the capture process. For full instructions see the
[Running on Android documentation](../docs/running_android.md).

## Behavior overrides

The following override groups are supported:

* **Feature:** control use of optional Vulkan features that might impact
  correctness and performance.
* **Serialization:** control serialization of GPU workload scheduling to
  diagnose issues caused by missing queue or command stream synchronization.
* **Shaders and Pipelines:** control shader pipeline compilation to diagnose
  issues caused by shader precision issues.
* **Framebuffers:** control use of lossy and lossless image compression for
  uncompressed images that could be used as framebuffer attachments.

### Features

The feature overrides allow forceful enable or disable of optional features
in the API.

#### Configuration options

The configuration file allows optional features to be force-enabled or
force-disabled. If no forced setting is enabled, application behavior is
left unmodified. Only a single force setting per feature should be set to
avoid ambiguous settings.

```jsonc
"feature": {
    "robustBufferAccess_enable": false, // Force enable robustBufferAccess
    "robustBufferAccess_disable": false // Force disable robustBufferAccess
},
```

### Serialization

The serialization overrides allow forceful serialization of submitted
workloads, ensuring that they run in queue submit order. The synchronization
is configured per workload type, allowing control over where serialization is
added to command buffers and queues.

#### Configuration options

The configuration file allows control over specific options, as well as
convenience options to force disable or enable all serialization.

* If the `none` option is `true` then no serialization is applied, irrespective
  of other settings.
* Else, if the `all` option is `true` then all serialization is applied,
  irrespective of other settings, with the exception of `queue_wait_idle` which
  must be enabled individually due to its extreme performance overhead.
* Else, the individual options are applied as specified.

```jsonc
"serialize": {
    "none": false,            // Enable no serialization options
    "all": false,             // Enable all serialization options except queue_wait_idle
    "queue": false,           // Force cross-queue serialization of submits
    "queue_wait_idle": false, // Insert vkDeviceWaitIdle after submits
    "commandstream": {
        "compute": {
            "pre": false,   // Insert full barrier before dispatches
            "post": false   // Insert full barrier after dispatches
        },
        "renderpass": {
            "pre": false,   // Insert full barrier before render passes
            "post": false   // Insert full barrier after render passes
        },
        "asbuild": {
            "pre": false,   // Insert full barrier before acceleration structure builds
            "post": false   // Insert full barrier after acceleration structure builds
        },
        "tracerays": {
            "pre": false,   // Insert full barrier before trace rays
            "post": false   // Insert full barrier after trace rays
        },
        "transfer": {
            "pre": false,   // Insert full barrier before transfers
            "post": false   // Insert full barrier after transfers
        }
    }
}
```

## Shaders and Pipelines

The shaders and pipelines overrides allow some control over how the shader
compiler handles compilation tasks.

#### Configuration options

```jsonc
"shader": {
    "disable_cache": false,              // Disable use of binary caches
    "disable_relaxed_precision": false,  // Disable use of relaxed precision decoration
    "enable_spirv_fuzz": false           // Enable SPIR-V fuzzing to change binary hash
}
```

### Shader dump

The layer can capture content-addressed SPIR-V modules and pipeline executable
statistics. The supplied configuration enables compact capture. When statistics
are requested the layer
enables `VK_KHR_pipeline_executable_properties` only if the device supports the
extension and its `pipelineExecutableInfo` feature. Unsupported devices still
dump SPIR-V modules and pipeline metadata.

```jsonc
"shader_dump": {
    "enable": true,
    "output_path": "/data/local/tmp/lgl-shader-dump",
    "mode": "compact",              // "compact" or "detailed"
    "capture_statistics": true,
    "capture_debug_utils": true,
    "usage_capture": "frame_summary", // "frame_summary" or "aggregate_only"
    "frame_start": 0,                  // First present-counted frame
    "frame_count": 300                 // 0 = no limit
}
```

When launched through `lgl_android_install.py --pull-artifact <host-directory>`,
the installer honors `shader_dump.enable`. When enabled it redirects output to
`/sdcard/Android/data/<package>/files/lgl-artifacts/shader-dump`. It then
pulls the shader data into a timestamped `<host-directory>/<YYYYMMDD_HHMMSS>_<package>`
directory and automatically generates `shader_usage_report.html` there when
capture finishes.

`mode` controls output size. `compact` is the default and retains the three report
inputs: `shaders/<SHA1>.spirv`, `pipelines/*.json`, and
`tables/frame_pipeline_usage.csv` on the device. On the host, the installer places
`frame_pipeline_usage.csv` and other raw CSV files directly in the capture directory,
alongside the report and its CSV exports, omitting `shader-dump/`, `tables/`,
and `summary/` wrappers. This is sufficient for SHA1 lookup, SPIR-V source
conversion, pipeline executable analysis, CPU-side hotness, and debug label
association. `detailed` additionally retains shader metadata JSON,
`shaders.csv`, raw executable CSV tables, per-frame JSON, and debug object
names for detailed capture diagnosis.

The pipeline table defaults to descending command count and hides pipelines
with no recorded commands or frames. Select `Show unused pipelines` to include
them. Search filters update the table while preserving input focus and cursor
position.

The overview separates capture totals from used-pipeline counts. Graphics and
compute counts refer to used pipelines. Toolbars show filtered result counts.
Both tables sort every column by clicking its entire header cell or pressing
Enter or Space on its button, preserving table scroll positions. SHA tooltips
show full identifiers; copy buttons and HTML Rank columns are omitted.
Opening a shader candidate highlights and focuses its visible shader row.
Escape closes detail panels.

The Help tab explains data scope, capture-relative flags, sorting, and fields.
Pipeline rows group identical shader combinations, not complete pipeline state.
Draw counts are CPU API call counts, including indirect, indirect-count, EXT mesh,
and EXT multi-draw commands supported by the framework. Each call counts once,
regardless of drawCount/maxDrawCount. `indirect_draw_command_count` is a subset of
draw_count; GPU-executed draw counts are unavailable without GPU buffer readback.
NV mesh and AMD-specific entrypoints not exposed by the common framework, and
device-generated-command pipeline attribution, are outside this coverage.
Details retain each captured pipeline instance and give every executable its own
column identified by instance ID and executable index. Summary metrics are maxima
across instances; usage and pass labels remain at shader-combination group level.
Statistics CSV exports include the instance ID and a capture-file instance key.
Flags are investigation hints, not verified GPU bottlenecks. Pipeline flag
buttons match any selected tag; Clear Tags resets only those selections.
The list-plus icon includes unused entries. Shader Type buttons filter stages.
Switching tabs preserves search, filters, and sort settings.

The Info tab beside Help focuses on device manufacturer/product, Android/SDK
and build fingerprint, package identity/version, GPU/driver identity and Vulkan
API versions, plus shader-related capture settings. Duplicate driver names are
omitted, and full configuration is collapsed. Session details, frame ranges,
tool identifiers and optional notes remain in metadata and Copy JSON.
`capture_info.json` stores the host snapshot, while `metadata/vulkan-*.json`
stores properties queried from the application's physical devices. Offline
regeneration uses these saved inputs without querying a connected device.

Package `versionName` and `versionCode` are read using
`dumpsys package <package>`, with Package Manager recorded as the source.
Version collection does not transfer APK files or require aapt2. Missing SoC,
driver or package fields do not prevent capture. Driver versions retain their
raw decimal/hex values; vendor-defined strings are preserved verbatim.

With `--pull-artifact`, optional installer flags `--capture-scene`,
`--capture-quality`, `--capture-resolution` and `--capture-note` record supplied
text in Info. `--capture-apk-hash` additionally pulls and hashes all installed
base/split APKs. Hashes are omitted by default to avoid the transfer cost;
enabling hashes does not change the source of version information.
Session times use the host clock with a UTC offset and include app launch/stop.
Completed denotes a normally ended session, not full coverage of all frames.
Recorded usage frame counts include only frame IDs present in usage CSV data;
the requested frame window is saved separately in the effective configuration.

The Shaders toolbar hides zero-inherited-command shaders by default. Enable
Show Unused Shaders to include them. Zero means no commands were recorded
for their referencing pipelines in this capture, not permanent non-use.
Shader pass groups are the deduplicated union from referencing pipelines;
inherited commands count each referencing pipeline once.

Report summaries are `pipeline_summary.csv` and `shader_summary.csv`, without
rank columns, plus `pipeline_shader_links.csv` and `pipeline_statistics.csv`.
`pipeline_signature` is a SHA1 of pipeline kind and shader stage/entry-point
identities, not full pipeline state. New captures use `frame_pipeline_usage.csv`; the report also accepts the legacy
`usage_by_label.csv` filename, preferring the new file if both exist.

Pipeline JSON preserves vendor-defined statistic names and values; tools must
not assume that a statistic called "registers" or "spills" is available on
every driver. With `frame_summary`, `frame_pipeline_usage.csv` contains one aggregate
record per `(frame, pipeline signature, debug label path)`. Counts are commands
recorded by the CPU, not GPU execution time. `VK_EXT_debug_utils` command buffer
label scopes are captured when the application uses that extension.

Generate an offline report with:

```sh
python layer_gpu_support/scripts/shader_dump_report.py <shader-dump-directory>
```

This reads `pipelines/` and `frame_pipeline_usage.csv` directly from the capture
directory and writes `shader_usage_report.html` and CSV exports there. Use `--output` to
choose a separate report directory. The report is intended
for devices supporting `VK_KHR_pipeline_executable_properties`; it retains the
vendor-defined statistics and currently recognizes QCOM and Mali conventions.

### Inspect shader code on demand

Select a shader in the report, click **Copy conversion command**, and run the
command in a terminal (PowerShell for reports generated on Windows). Only that
shader is converted; the report is not regenerated. The default formats are
reconstructed Vulkan GLSL and SPIR-V assembly:

```sh
python layer_gpu_support/scripts/shader_dump_report.py <capture-directory> --convert-shader <SHA1> --formats glsl spvasm
```

Add `hlsl` to `--formats` for reconstructed HLSL (shader model 6.0). Conversion
requires `spirv-cross` and `spirv-dis` on PATH, for example from Vulkan SDK Bin.
Use `--spirv-cross <executable>` and `--spirv-dis <executable>` to specify paths.
Failures in one format do not prevent conversion of the remaining formats;
unsupported shaders can still be inspected as SPIR-V assembly.

Results are saved under `<capture-directory>/shader_sources/<SHA1>.glsl`,
`.hlsl`, and `.spvasm`. Modules with multiple entry points get separate GLSL/HLSL
files with stage and entry-point suffixes. Existing files are reused; `--force`
regenerates them. Both `.spirv` and `.spv` binary inputs are accepted.

For frequent code browsing, pass `"*"` instead of a SHA1 to convert all shaders
once before returning to the report (quote `*` to prevent shell wildcard expansion):

```sh
python layer_gpu_support/scripts/shader_dump_report.py <capture-directory> --convert-shader "*" --formats glsl hlsl spvasm
```

Batch conversion uses multiple worker processes automatically, defaulting to the
CPU logical core count (and no more workers than unique shaders). Each worker handles one shader at a time, with its requested formats
converted in sequence. Use `--jobs 4` to set the worker count, or `--jobs 1` for
serial conversion. At most one worker is assigned to each unique shader, so
duplicate binaries do not write the same output concurrently. Completion logs
are grouped per shader and progress is reported in completion order.

`--convert-shader "*"` scans `<capture-directory>/shaders/`. Alternatively pass a folder
containing `.spv` or `.spirv` files directly; its results go into that folder's
`shader_sources/` directory. Scanning is not recursive. Outputs use the binary
content SHA1 even when input filenames are arbitrary, and duplicate binaries are
converted only once. Cached results are reused unless `--force` is supplied.
Progress and a final summary are printed. Individual failures do not stop the
batch; the command exits with a nonzero status if any shader failed. This mode
does not regenerate HTML or convert files automatically during ordinary capture.
In the report, select the resulting `shader_sources` folder once, then browse
shaders normally; use **Refresh code** if the folder was already selected.

Shader details open on **Referencing Pipelines**; switch to **Code** to inspect
source without scrolling past long code listings to reach pipeline information.
Click **Load source folder** and select `shader_sources` once. Files are indexed
by SHA1 and only the selected shader's code is read. Switching shaders automatically
loads their matching files, so there is no need to choose each `.glsl` individually.
In browsers supporting directory handles, returning from the terminal or clicking
**Refresh code** scans that folder for new conversions. Other browsers use a folder
upload picker; select the folder again to discover new files. Access is read-only
and files stay on the computer. Folder access and loaded code last only until the
page is refreshed or closed. Select the folder again when reopening the report.

**Load files** remains available for individual files and rejects filename SHA1s
that do not match the selected shader. The viewer provides language tabs, line
numbers, search highlighting, and offline syntax coloring for GLSL, HLSL,
and SPIR-V assembly. `.spvdis` is also accepted as an assembly filename. Files
larger than 10 MiB are rejected. Source text is HTML-escaped before highlighting;
it is never executed. No network or local server is required.

GLSL/HLSL are reconstructed from the captured module, not the original developer
source. Pipeline specialization constants are not applied by this conversion.

## Framebuffers

The framebuffer overrides allow some control over how the framebuffers are
allocated and handled by the driver.

* If the `disable_compression` option is `true` then compression is always
  disabled, irrespective of other settings.
* Else, if the `force_default_compression` option is `true` then all images
  will be forced to use default compression (i.e. stripping all application use
  of fixed rate compression), irrespective of the fixed rate setting.
* Else, the `force_fixed_rate_compression` will be used to force fixed rate
  compression as close as possible (but no lower) than the specified bits
  per component setting. Images that do not support a fixed rate compression
  level that meets this bit rate requirement will be left at the original
  application setting.

#### Configuration options

```jsonc
"framebuffer": {
    "disable_compression": false,        // Disable all use of compression
    "force_default_compression": false,  // Force driver default compression
    "force_fixed_rate_compression": 0    // Force use of fixed rate compression as close
                                         // to this bits-per-channel as possible, but
                                         // no lower (0 = do not force)
}
```

- - -

_Copyright © 2025, Arm Limited and contributors._
