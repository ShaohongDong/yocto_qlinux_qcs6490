# Q6A GPU performance benchmark

GPU rendering baseline app with GTK3 controls and a headless CLI. It uses
OpenGL ES 3, EGL and a GBM render node; it does not change clocks or governors.
Includes a real rotating, textured 3D Flight Helmet model. Only Adreno/Freedreno hardware renderers are accepted. No FPS threshold is
implied by a successful run.

## Build

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app app005-gpu-benchmark --machine radxa-dragon-q6a
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app build --app app005-gpu-benchmark --machine radxa-dragon-q6a
```

The existing selected-app recipe builds the package. An optional future image
build uses `scripts/qcom-app image --app app005-gpu-benchmark --machine radxa-dragon-q6a`.
Only selecting this app adds its Weston launcher. No service starts a GPU load
automatically at boot.

## Run

```sh
gpu-benchmark --gui
gpu-benchmark --headless --suite quick --output ./gpu-quick
gpu-benchmark --headless --suite stability --output ./gpu-stability
gpu-benchmark --headless --suite model3d --instances 16 --output ./gpu-3d
gpu-benchmark --gui --window --suite stability --autorun --quit-after-run --output ./gpu-window
gpu-benchmark-acceptance ./gpu-board-acceptance
```

Default render size: 1920x1080. `--size WIDTHxHEIGHT` accepts each dimension
from 64 to 4096. Quick runs geometry, texture, fragment, mixed and model3d scenes for
10 seconds each. Stability runs mixed for **60 seconds**; model3d runs the 3D model
for **60 seconds**. Every scene has an
additional 3-second warmup. `--duration 1..3600` changes measurement seconds per
scene. `--device /dev/dri/renderD128` selects the offscreen GPU when multiple
render nodes exist. Without an explicit selection exactly one node is required.

The GUI defaults to the 3D model and window mode; `--gui --offscreen` selects the
offscreen mode at startup. The GUI has independent offscreen and window modes, suite and duration controls,
live metrics, Start/Stop and an Open results folder button. Window mode previews
the actual workload. Offscreen mode runs a child process without preview so the
desktop presentation cadence cannot cap its throughput. Each GUI run creates a
new result subdirectory. GUI automation flags are provided for board smoke tests.
Default GUI reports go under `$XDG_DATA_HOME/gpu-benchmark/results` (normally
`~/.local/share/gpu-benchmark/results`). GDK is initialized with `GDK_GL=gles`
unless explicitly configured by the caller: Qualcomm's GLES-only EGL stack
cannot create GDK's default shared desktop OpenGL context. GL creation failures
are reported and terminate automated runs even if GTK never emits `render`.

## 3D model scene

Flight Helmet is downloaded from Khronos glTF Sample Assets, pinned to the commit
and SHA256 in `data/models/provenance.json`. It is CC0-1.0; upstream attribution
and the license are included. The cgltf v1.15 parser is MIT licensed.

The model uses perspective projection, depth testing, two directional lights,
specular highlights, sRGB base-color textures, and automatic rotation. It renders
1–64 instances selected with `--instances` or the GUI control. The report records
model SHA256, triangle count per instance and instance count. `--model PATH.glb`
selects another self-contained static GLB; `--check-model --model PATH.glb` checks
geometry without creating a GPU context. The UI scene label identifies the bundled
model; a custom file is selected through CLI and identified in its report.

This is simplified material lighting, not a full glTF PBR viewer. The packaged
asset retains geometry and base-color textures; normal/occlusion/metallic-roughness
maps and optional transmission are omitted. Custom models must have triangles,
normals and UV0 for embedded base-color textures. Skinning, morph targets, required
extensions, external resources, transparent/masked materials and texture transforms
are rejected. Limits: 128 MiB GLB, 2 million triangles and 4096x4096 textures.
The GLB is installed under `/usr/share/gpu-benchmark/models/`; a portable deployment
can instead place `models/FlightHelmet.glb` beside the executable.

## Workload and measurement contract (version 2)

- Fixed 128x128 triangle grid: 32,768 triangles and 98,304 vertices per frame.
- Geometry: interpolated color gradient. Texture: 16 bilinear samples from a
  deterministic 256x256 RGBA texture. Fragment: 64 dependent vector sine
  iterations. Mixed combines texture and fragment work on the same geometry.
- Each frame renders to an RGBA8 FBO of the requested size. CPU frame time spans
  submission through GPU fence completion, excluding presentation and reporting.
  The fence is polled with a 5-second timeout. This is a synchronized workload
  baseline, not theoretical peak hardware throughput or a glmark2 score.
- GPU time is separately recorded only when `GL_EXT_disjoint_timer_query` is
  available and the result is ready and non-disjoint; otherwise it is null.
- FPS uses completed measured frames divided by monotonic elapsed time, including
  telemetry/reporting and (in window mode) desktop scheduling. Window FPS is the
  application render cadence, not a measurement of physical scanout or latency.
  The FBO is scaled to the preview area; its render resolution remains fixed.
- Warmup and deterministic constant-color/gradient pixel checks are excluded
  from metrics. Pixel checks run before and after a completed suite.
  Shader/pixel errors, rejected software rendering or timeout fail
  the run. SIGINT/SIGTERM and GUI Stop preserve partial results as `cancelled`.
- Per-frame mean and nearest-rank P95/P99, valid GPU timing sample counts, per-second
  FPS, read-only GPU frequency and all readable thermal-zone values are recorded.
  Optional busy percentage is read only when its sysfs file exists; missing
  telemetry is null. Thermal-zone names are retained without guessing which is GPU.
- `passed` means completed without detected rendering errors; performance remains
  `baseline_only`. A 60-second pass establishes only short-run stability.

`summary.json` is atomically replaced at checkpoints and completion. `samples.csv`
contains per-second measurements; `thermal.csv` includes named thermal-zone samples.
`summary.txt` provides a readable per-scene summary, also shown in the GUI when
the run completes or is cancelled.
Scene summaries compare the first/last up to five samples (at most half the
available samples each; intervals below 0.9 seconds are excluded), reporting a
ratio without inferring thermal throttling.
Reports include renderer, driver/API version,
device, mode, scene version, timestamps, parameters and status. Do not compare
offscreen and window numbers as equivalent tests. Keep board kernel/build,
desktop/background load and kernel log evidence alongside reports.

## Tests and evidence

```sh
cmake -S apps/app005-gpu-benchmark -B /tmp/gpu-core -DGPU_CORE_ONLY=ON
cmake --build /tmp/gpu-core
ctest --test-dir /tmp/gpu-core --output-on-failure
```

With GTK3/EGL/GLES/GBM development packages, omit `GPU_CORE_ONLY` to run CLI
failure tests and the graphics-independent `--self-test`. The same self-test is
suitable for QEMU user-mode. Neither host tests nor QEMU establish board GPU
performance. Board acceptance must include actual renderer identity, quick and
60-second results, desktop checks and accessible GPU error logs.
The acceptance helper runs the five-scene quick suite and the 60-second model3d
suite sequentially with a 90-second
watchdog each, saves kernel logs and background-process context, and records
permission errors if kernel logs cannot be read. Review new GPU errors separately;
application `passed` alone does not assert that the kernel log is clean. A forced
process kill can leave a checkpoint marked `running`; it is never acceptance.
