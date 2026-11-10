#pragma once

#include "core/system/Config.hpp"
#include "core/system/CommandLineArgs.hpp"


namespace sibr {
    /// Arguments for the foveated renderer application.
    struct VRHTGSViewerAppArgs :
        virtual BasicIBRAppArgs {
        RequiredArg<std::string> modelPath = { "model-path", "Model directory" };
        RequiredArg<std::string> modelPathShort = { "m", "Model directory" };
        Arg<int> device = {"device", 0, "CUDA device index"};
        Arg<bool> noInterop = { "no-interop", "Don't try to use interop (may be required for unconventional OpenGL setups, like WSL)" };
        Arg<bool> exportVisibilityMask = { "export-visibility-mask", "Export the visibility mask for each eye as a line loop (for debugging and use in NeRFICG framework)" };
    };

    struct VRHTGSExperimentAppArgs :
        virtual BasicIBRAppArgs {
        RequiredArg<std::string> modelPath = { "model-path", "Model directory" };
        RequiredArg<std::string> modelPathShort = { "m", "Model directory" };
        Arg<int> device = {"device", 0, "CUDA device index"};
        Arg<bool> noInterop = { "no-interop", "Don't try to use interop (may be required for unconventional OpenGL setups, like WSL)" };
        Arg<int> participantID = { "participant-id", -1, "Participant ID for experiment" };
        Arg<int> startFromCondition = { "start-from-condition", 0, "Condition index to start from (0 = first condition)" };
        Arg<bool> calibrationMode = { "calibration-mode", "Run in calibration mode (allow adjustment of scene scale + position and write calibration results to file in dataset dir)" };
        Arg<bool> exportVisibilityMask = { "export-visibility-mask", "Export the visibility mask for each eye as a line loop (for debugging and use in NeRFICG framework)" };
        Arg<bool> english = { "english", "Display Questionnaire in English (default is German)" };
    };
}

