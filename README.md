# SIBR Core

**SIBR** is a System for Image-Based Rendering.
It is built around the *sibr-core* in this repo and several *Projects* implementing published research papers.
For more complete documentation, see here: [SIBR Documentation](https://sibr.gitlabpages.inria.fr)

This **SIBR core** repository provides :
- a basic Image-Based Renderer
- a per-pixel implementation of Unstructured Lumigraph (ULR)
- several dataset tools & pipelines do process input images

Details on how to run in the documentation and in the section below.
If you use this code in a publication, please cite the system as follows:

```
@misc{sibr2020,
   author       = "Bonopera, Sebastien and Esnault, Jerome and Prakash, Siddhant and Rodriguez, Simon and Thonat, Theo and Benadel, Mehdi and Chaurasia, Gaurav and Philip, Julien and Drettakis, George",
   title        = "sibr: A System for Image Based Rendering",
   year         = "2020",
   url          = "https://gitlab.inria.fr/sibr/sibr_core"
}
```
## OpenXR

This branch supports headed-mounted displays through [OpenXR](#use-a-vr-headset).

## Command Line Setup and Build

Open the repository in `x64 Native Tools Command Prompt for VS 2022` and type:

```cmd
cmake --preset vcpkg-release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -S . -B build
# or use --preset vcpkg-debug to build in debug mode
cmake --build build
```


## Setup

**Note**: The current release is for *Windows 11* only. Please note that Visual Studio with C++23 standard is required to compile.

#### Binary distribution

The easiest way to use SIBR is to download the binary distribution. All steps described below, including all preprocessing for your datasets will work using this code.

Download the distribution from the Releases page: TODO: Add Link (Core, ???Mb); unzip the file and rename the directory "install".
You can also grab a sample model from there, or train your own using [VRHTGS](https://github.com/nerficg-project/VRHTGS).

#### Install requirements

All version numbers are the version used in our testing. Other versions of the same software may work, but we cannot offer any support when using different versions.

TODO: Mention vcpkg and how to modify CMakeUserPresets.json.template

- [**Visual Studio 2022**](https://visualstudio.microsoft.com/downloads/)
  (make sure to install the Desktop development with C++ workload as well as the recommended MSVC build tools)
- [**Cmake 4.3.2**](https://cmake.org/download)
- [**7zip**](https://www.7-zip.org)
- [**CUDA 12.8**](https://developer.nvidia.com/cuda-downloads)

Make sure CUDA is in the PATH, and the `CUDA_HOME` and `CUDA_PATH` environment variables are set to the correct installation directory.

#### Generation of the solution
TODO: Update with the correct URLs & Build Instructions

- Checkout this repository's master branch:
  ```sh
  ## through HTTPS
  git clone https://gitlab.inria.fr/sibr/sibr_core.git -b master
  ## through SSH
  git clone git@gitlab.inria.fr:sibr/sibr_core.git -b master
  ```
- Run Cmake-gui once, select the repo root as a source directory, `build/` as the build directory. Configure, select the Visual Studio C++ Win64 compiler
- Select the projects you want to generate among the BUILD elements in the list (you can group Cmake flags by categories to access those faster)
- Generate

#### Compilation

- Open the generated Visual Studio solution (`build/sibr_projects.sln`)
- Build the `ALL_BUILD` target, and then the `INSTALL` target
- The compiled executables will be put in `install/bin`
- TODO: are the DLLs properly installed?

## Troubleshooting

#### Cmake complaining about the version

if you are the first to use a very recent Cmake version, you will have to update `CHECKED_VERSION` in the root `CmakeLists.txt`.

#### Weird OpenCV error

you probably selected the 32-bits compiler in Cmake-gui.

#### `BUILD_ALL` or `INSTALL` fail because of a project you don't really need

build and install each project separately by selecting the proper targets.

## To run an example
TODO: Update this and the VR section with example usage


### Support for VR headsets using OpenXR (provided by Orange)


* The new SIBR rendering mode `OpenXRRdrMode` supports head-mounted display (HMD) OpenXR devices.
* The VR-HTGS viewer (`SIBR_FoveatedHTGSViewer_App`) uses this rendering mode with the `--rendering-mode 2` option (the default) to render a scene to a two-view headset display through the OpenXR stack.
* This mode currently works on Windows only.
* Further, we've only tested the eye-tracking support with a Meta Quest Pro using the Meta Horizon Link software. **Eye tracking may not work on other headsets using the current code base.**

---
**How to test:**

**Windows (Meta Quest Pro):**

1. Install the Desktop PC Meta Horizon Link App and connect your headset: https://www.meta.com/en-us/help/quest/1517439565442928
2. For eye tracking support, you need to enable developer mode in *both* the headset settings *and* the Meta Horizon Link App. Under the developer settings in the Meta Horizon Link App, you should then be able to activate Eye-Tracking via Meta Horizon Link.
3. We recommend connecting the headset with an official Meta Quest Link cable for stability, as eye tracking sometimes breaks when using AirLink.
4. Make sure you've activated the link connection in the headset (The headset should be in the Oculus Link Home screen (white grid background)).
5. Run `SIBR_FoveatedHTGSViewer_App -m <model_dir> --rendering-mode 2`

TODO: Update the remaining part of this section
![openxr gaussian viewer](./docs/img/openxr_gaussian_viewer.png)

5. You can try `Free world standing` and `Seated` VR experiences
6. If you are experiencing lags in the headset, try to lower the rendering resolution by changing the `Down scale factor` slider value.
