# Foveated SIBR: a VR viewer for VR-HTGS

This repository is a fork of [SIBR](https://sibr.gitlabpages.inria.fr) (System for Image-Based Rendering, Inria)
that builds on their `gaussian_code_release_openxr` branch to provide a VR viewer and a perceptual-study app for
**VR-HTGS**, from the paper *Fast Eye-Tracked 3D Gaussian Rendering for Virtual Reality*
([project page](https://www.timonscholz.dev/paper/vr-htgs),
[paper / supplemental](https://diglib.eg.org/items/0155e618-7d14-4dd0-b864-1dbbf431ec1b),
[method code](https://github.com/nerficg-project/VRHTGS)).

The fork provides two applications:

- **`SIBR_FoveatedHTGSViewer_App`**: a standalone viewer for HTGS / VR-HTGS models, on the desktop or in an OpenXR
  headset with eye-tracked foveated rendering.
- **`SIBR_FoveatedHTGSExperiment_App`**: the app used for our user study. It plays a randomized list of conditions in
  the headset, asks an in-VR questionnaire after each one and records every rendered frame.

Models are trained with the [nerficg](https://github.com/nerficg-project/nerficg) framework and the
[VRHTGS](https://github.com/nerficg-project/VRHTGS) method, then exported for this viewer using the
`vrhtgs_export_model.py` script provided in the main [VRHTGS repository](https://github.com/nerficg-project/VRHTGS). For the
experiments, configurations are generated using the `vrhtgs_create_experiment_config.py` script, also provided there.
For details on the script usage, refer to the scripts'
[README.md](https://github.com/nerficg-project/VRHTGS/blob/master/scripts/README.md).

> **Note:**
> **The performance and quality results in the paper were measured with the nerficg implementation of VR-HTGS
> ([nerficg-project/VRHTGS](https://github.com/nerficg-project/VRHTGS/)), not with this SIBR viewer.**
> To reproduce them, use the benchmark scripts in that repository. Numbers from this viewer are not comparable,
> because:
>
> - the SIBR viewer is not as optimized as the nerficg code path (it adds the overhead of SIBR's OpenGL/OpenXR
>   rendering loop and CUDA–OpenGL interop, and the Experiment app additionally records every frame for post hoc analysis);
> - on this branch, the SIBR viewer does **not** use the faster stereo rendering path of VR-HTGS, which renders
>   both eyes in a single pass. It renders each eye separately with the mono rasterizer. Experimental stereo rendering
>   support is available on the [`stereo-rendering`](https://github.com/nerficg-project/VRHTGS-SIBR/tree/stereo-rendering)
>   branch, where both apps use it by default in VR.

## Changes to the SIBR framework

Compared to upstream SIBR core, our main changes include:

- **OpenXR support** (`src/core/openxr/`, originally contributed by Orange on the `gaussian_code_release_openxr` branch of the upstream [SIBR](https://sibr.gitlabpages.inria.fr) repository): the `OpenXRRdrMode` rendering mode
  renders to a head-mounted display. This fork extends the OpenXR wrapper with
  - eye tracking (`XR_FB_eye_tracking_social`),
  - visibility masks, so that areas hidden by the lens geometry are skipped during rendering,
  - controller actions (sticks and buttons),
  - a fixed FOV calculation, and robust handling of missing or lost OpenXR instances (the app reports
    "Failed to connect to OpenXR" and exits instead of hanging),
  - a frame-begin callback that exposes OpenXR's predicted display time and period.
- **Build system**:
  - moved dependency management to [vcpkg](https://vcpkg.io) (manifest `vcpkg.json`, static triplet `x64-windows-static`)
    instead of SIBR's downloaded binary packages;
  - added CMake presets (`CMakePresets.json`) using Ninja;
  - removed the Doxygen documentation build to avoid shipping out-of-date documentation;
  - **Windows only**: removed Linux support and dynamic linking, since the Meta Quest headsets we target are
    not supported under Linux anyway.
- **Modernized code base**: C++23 (`std::filesystem` instead of `boost::filesystem`, `std::print`, standard
  integer types), and fixes for newer versions of ImGui and nativefiledialog.
- **Desktop camera**: a new `ICGUI` mode of `InteractiveCameraHandler` (`src/core/view/ICGuiCamera.*`) that mimics
  the camera controls of nerficg's ICGui (orbital, walking and flying modes) to provide more intuitive controls in
  the desktop viewer.

## Setup

**Note**: This fork runs on *Windows 11* only and needs Visual Studio 2022 with C++23 support and an NVIDIA GPU with
CUDA. The CUDA backends are compiled for the build machine's GPU by default. For binaries that run on other GPUs, pass
a list, e.g. `-DVRHTGS_CUDA_ARCHITECTURES="61-real;75-real;86-real;89-real;120-real;120-virtual"`.

### Binary distribution

The easiest way to use the viewer is to download the binary distribution.

Download the distribution from the Releases page \[Coming Soon] <!-- TODO: Add Link -->,
unzip the file and rename the directory to "install".
You can also grab a sample model from there, or train your own using [VRHTGS](https://github.com/nerficg-project/VRHTGS).

### Requirements

All version numbers are the versions used in our testing. Other versions may work, but we cannot offer any support for them.

- [**Visual Studio 2022**](https://visualstudio.microsoft.com/downloads/)
  (with the *Desktop development with C++* workload and the recommended MSVC build tools)
- [**CUDA Toolkit 12.8**](https://developer.nvidia.com/cuda-downloads)
- [**CMake 4.2.3**](https://cmake.org/download)
- [**vcpkg**](https://learn.microsoft.com/vcpkg/get_started/get-started)

Make sure CUDA is in the `PATH`, and that the `CUDA_HOME` and `CUDA_PATH` environment variables point to the CUDA
installation directory.

### Building from source

1. Clone the repository with its submodules (the two CUDA rasterizer backends):
   ```sh
   git clone --recursive https://github.com/nerficg-project/FoveatedSIBR.git
   # or, in an existing clone:
   git submodule update --init
   ```
2. Copy `CMakeUserPresets.json.template` to `CMakeUserPresets.json` and set `VCPKG_ROOT` to your vcpkg installation.
3. Open the repository in the `x64 Native Tools Command Prompt for VS 2022` and run:
   ```sh
   cmake --preset vcpkg-release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -S . -B build
   # or use --preset vcpkg-debug to build in debug mode
   cmake --build build --target SIBR_FoveatedHTGSViewer_App SIBR_FoveatedHTGSExperiment_App
   ```
   The first configure builds all vcpkg dependencies, which takes a while.

The executables are linked directly into `install/bin/`; no separate install step is needed.
After adding or removing source files, re-run the configure step, since the source lists are collected with
`file(GLOB ...)`.

The CUDA backends are git submodules:

| Path | Repository | Rasterizer |
|---|---|---|
| `src/projects/vrhtgs_viewer/renderer/HTGSCudaBackend/HTGS` | [nerficg-project/HTGS](https://github.com/nerficg-project/HTGS) | `HTGS` (non-foveated) |
| `src/projects/vrhtgs_viewer/renderer/VRHTGSCudaBackend/VRHTGS` | [nerficg-project/VRHTGS](https://github.com/nerficg-project/VRHTGS) | `HTGS_FOVEATED` (VR-HTGS) |

Only their plain CUDA sources are compiled (without the PyTorch bindings), through the small `CMakeLists.txt` next
to each submodule.

## Preparing a model

Train a model with nerficg and VRHTGS (see the [VRHTGS README](https://github.com/nerficg-project/VRHTGS)), then
export it with

```sh
python ./scripts/vrhtgs_export_model.py -d <model dir>
```

A model directory for this viewer contains:

- `final.ply` or `gaussians.ply` (standard 3DGS layout; `gaussians.ply` wins if both exist, otherwise the first
  loadable `.ply` is used);
- optionally `training_config.yaml`, from which only the near/far planes and the dataset name are read. The training
  dataset itself is **not** needed, so models can be shared on their own;
- optionally a background model: `background_sh_coefficients.bin` (`BACKGROUND_MODEL: SH`),
  `background_texture.png` (`BACKGROUND_MODEL: MLP`, baked) or `background_texture.exr` (`BACKGROUND_MODEL: TEX`).
  Background models are only used by the VR-HTGS (`HTGS_FOVEATED`) rasterizer.

## Viewer: `SIBR_FoveatedHTGSViewer_App`

```cmd
SIBR_FoveatedHTGSViewer_App.exe -m <model dir> [--rendering-mode 0|2] [--device <cuda index>] [--export-visibility-mask]
```

- `--rendering-mode 2` (default) renders to an OpenXR headset, `--rendering-mode 0` to a desktop window.
- `--rendering-size W H` sets the desktop rendering resolution (default 1280×720, 60° vertical FOV). In VR the
  headset's resolution and FOV are used.
- `--export-visibility-mask` writes each eye's visibility mask as a line loop (`render_mask_view_0.bin` /
  `render_mask_view_1.bin`, as `float2` points in [0, 1]) into the working directory, e.g. for use in nerficg
  to emulate your specific headset's visibility mask shape.

The **3D Gaussians** window lets you toggle anti-aliasing and peripheral blur, switch between the `HTGS` and
`HTGS_FOVEATED` rasterizers, and adjust height and locomotion speed. The **OpenXR** window switches between seated
(default) and free-standing experiences, shows the frame rate and lets you lower the rendering resolution if the
headset lags.

**Desktop controls** (nerficg ICGui style): orbital by default (left drag orbits, right drag pans, wheel zooms).
Alt or End cycles to walking / flying (WASD, R/Space up, F/Shift/Ctrl down, left drag looks); +/− change the speed.
In the seated VR experience the desktop camera position offsets the VR view.

**Controllers**: the left stick moves / strafes relative to the head, the right stick turns (x) and moves up / down
(y), and Y/B resets the position.

## Experiment: `SIBR_FoveatedHTGSExperiment_App`

```cmd
SIBR_FoveatedHTGSExperiment_App.exe -m <config prefix> --participant-id <id> [--calibration-mode] [--calibration-dir <dir>] [--english] [--start-from-condition <n>] [--no-recording]
```

- Experiment configurations are generated using the `vrhtgs_create_experiment_config.py` script provided in the
  [VRHTGS](https://github.com/nerficg-project/VRHTGS/) repository. For details on the script usage, refer to the
  [README.md](https://github.com/nerficg-project/VRHTGS/blob/master/scripts/README.md) of that repository.
- `-m` is the **prefix** of the experiment configuration path: the app reads `<prefix>_<participant-id>.csv`. Each
  line is one condition: `MODEL_TYPE,MODEL_PATH,ANTI_ALIASING,BLUR_PERIPHERY` (`MODEL_TYPE` is `HTGS` or `VRHTGS`,
  the flags are `True` / `False`).
- Only VR (`--rendering-mode 2`) is supported.
- Each condition shows the scene (A/X ends it) followed by a 7-item questionnaire (stick left/right rates, A/X goes to
  the next question, B/Y goes back). The first condition starts with a gaze-dot check. The questionnaire is German by
  default; `--english` switches to English.
- The experimenter window on the desktop mirrors the headset and has buttons for each step. Escape aborts the run.
- `--start-from-condition <n>` skips the first `n` conditions, e.g. to resume an interrupted session.
- **Calibration**: With `--calibration-mode`, the sticks adjust the model scale as well as the starting rotation and
  position (Y/B cycles through them), and the result is written to `<calibration dir>/<dataset>/` as `calibration.txt`
  and `default_pose.txt`. The calibration directory defaults to `calibrations/` next to the config CSV. This allows
  matching the scene to the room in which the experiments are conducted. Datasets without a calibration produce a warning at
  startup and are shown at scale 1 with the identity pose.

The app writes into the current working directory:

- `experiment_log_participant_<id>_<timestamp>.txt`: a human-readable log of the conditions and ratings (−3..+3);
- `experiment_recording_participant_<id>_<timestamp>.htgsrec`: a binary recording of every rendered frame (poses,
  gaze, OpenXR display timing). The format is documented in `src/projects/vrhtgs_viewer/renderer/Recording.hpp`. Pass `--no-recording` to skip it.

## VR headsets and eye tracking

- The OpenXR mode (`OpenXRRdrMode`) currently works on Windows only.
- We have only tested eye tracking with a **Meta Quest Pro** using the Meta Horizon Link software.
  **Eye tracking may not work on other headsets with the current code base.**

**How to set up the Meta Quest Pro for eye tracking:**

1. Install the Meta Horizon Link app on the PC and connect your headset: https://www.meta.com/en-us/help/quest/1517439565442928
2. For eye tracking, enable developer mode in *both* the headset settings *and* the Meta Horizon Link app. In the
   developer settings of the Link app you can then enable eye tracking over Meta Horizon Link.
3. We recommend an official Meta Quest Link cable, since, in our testing, eye tracking sometimes stopped working over Air Link.
4. Start the Link connection in the headset and wait until the Link home environment (white grid) is shown.
   Otherwise the app reports "Failed to connect to OpenXR" and exits.
5. Run `SIBR_FoveatedHTGSViewer_App -m <model dir> --rendering-mode 2`.

Eye-tracking availability depends on matching headset and Link software versions; updating both has fixed it for us
before. Only one OpenXR app can use the headset at a time.

The experiment shows a gaze dot before the first trial, which can be hidden with A/X. If the dot doesn't move, your
eye-tracking setup is likely broken, and you may need to double-check your Quest Pro developer settings.

## Troubleshooting

#### CMake complains about the version

If you use a very recent CMake version, pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (as above) or update
`CHECKED_VERSION` in the root `CMakeLists.txt`.

#### OpenCV or linker errors

You probably configured from a 32-bit or non-MSVC shell. Use the `x64 Native Tools Command Prompt for VS 2022`.

#### "an illegal memory access was encountered"

CUDA errors are reported asynchronously. Run the viewer in `--rendering-mode 0` under
`compute-sanitizer --tool memcheck` to find the faulting kernel.

## License and Citation

This fork builds on the SIBR code base and is therefore distributed under SIBR's license, the
[Apache License 2.0](LICENSE.md). This includes the OpenXR support originally contributed by Orange, as well as our
changes to the SIBR core and the `vrhtgs_viewer` project.

The following parts have their own licenses:

- The HTGS and VRHTGS CUDA backends (the git submodules in
  `src/projects/vrhtgs_viewer/renderer/HTGSCudaBackend/HTGS` and
  `src/projects/vrhtgs_viewer/renderer/VRHTGSCudaBackend/VRHTGS`) are licensed under the **MIT License**. See the
  `LICENSE` file in each submodule.
- The questionnaire font (Open Sans, `src/projects/vrhtgs_viewer/renderer/fonts/`) is licensed under the
  SIL Open Font License 1.1 (`OFL.txt`).
- Third-party dependencies installed through vcpkg are subject to their respective licenses.

If you use this code for your research projects, please consider citing the VR-HTGS paper and the SIBR system:

```bibtex
@inproceedings{scholz2026vrhtgs,
    title = {{Fast Eye-Tracked 3D Gaussian Rendering for Virtual Reality}},
    author = {Scholz, Timon and Hahlbohm, Florian and Eisemann, Martin and Castillo, Susana and Magnor, Marcus},
    booktitle = {Vision, Modeling, and Visualization (VMV)},
    publisher = {The Eurographics Association},
    editor = {Leimkühler, Thomas and Weinkauf, Tino and Campen, Marcel},
    year = {2026},
    DOI = {10.2312/vmv.20261013},
    ISBN = {978-3-03868-318-6}
}
```

```bibtex
@misc{sibr2020,
   author       = "Bonopera, Sebastien and Esnault, Jerome and Prakash, Siddhant and Rodriguez, Simon and Thonat, Theo and Benadel, Mehdi and Chaurasia, Gaurav and Philip, Julien and Drettakis, George",
   title        = "sibr: A System for Image Based Rendering",
   year         = "2020",
   url          = "https://gitlab.inria.fr/sibr/sibr_core"
}
```
