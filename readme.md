# Welcome to Detached FSR/DLSS Framework

This repository showcases a [sample](./samples/fsr/) implementation of the Detached FSR/DLSS Framework. It is possible to run the sample without detaching the upscaler as well. You can access the documentation for the individual methods used below:

-   [FSR (FidelityFX SDK)](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/55ff22bb6981a9b9c087b9465101769fc0acd447/readme.md)
-   [DLSS (Streamline SDK)](https://github.com/NVIDIAGameWorks/Streamline/blob/c709dd9874e21dea100d6e2f2e109d16b87b8b55/README.md)

## How to build the sample

Building the sample is exactly the same as building the FidelityFX SDK. You can find the instructions [here](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/release-FSR3-3.0.4/docs/getting-started/building-samples.md). Be sure to build "Native-backed DLL" version.

To build our modified sample you should follow these instructions:

1. Install the following software developer tool minimum versions:

-   [CMake 3.17](https://cmake.org/download/)
-   [Visual Studio 2019](https://visualstudio.microsoft.com/downloads/)
-   [Windows 10 SDK 10.0.18362.0](https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk)
-   [Vulkan SDK 1.3.239](https://vulkan.lunarg.com/) (Not used in the sample, but required for the FidelityFX SDK)

2. Generate Visual Studio FSR sample solutions:

**Native-backend DLL version**

```bash
> <installation path>\BuildFSRSolutionNativeDll.bat
```

This will generate a `build\` directory where you will find the solution for either the native-backend-backed SDK samples (`FidelityFX SDK Native.sln`).

Also be sure to download media files using the following command:

```bash
> <installation path>\UpdateMedia.bat
```

> More information about this tool can be found [here](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/release-FSR3-3.0.4/docs/tools/media-delivery.md).

## A note on the sample

Detaching the upscaler from the rendering process requires both process to be in sync. To account for scheduling issues and not to drop any rendered frames, both processes will wait for each other to fill/empty the resource pool. The resource pool is a static pool of 10 buffers. Each buffer has enough space for all the required resources for both FSR and DLSS to function.

> It is possible to optimize this aspect by using a dynamic resource pool. This will allow the upscaler to run at a different rate than the renderer. However, at this time this sample is no more than a proof of concept.

## How to run the sample

Since we deploy two seperate processes for the upscaling and rendering, it becomes difficult to run it within the Visual Studio IDE. We recommend building the solution and the use the helper CLI tool ([governor](./governor.py)) to run the sample.

1. Build the solution using the instructions above.
2. Run the sample using the following command:

```bash
# Display help
python governor.py -h

# Example usage
python governor.py --render-res 1285 835 --upscaler FSR3

# Example usage without detaching the upscaler
python governor.py --render-res 1285 835 --upscaler FSR3 --use-default

# Example usage if you want to launch the upscaler from Visual Studio
python governor.py --render-res 1285 835 --upscaler FSR3 --skip-upscaler # This will skip launching the upscaler
```
