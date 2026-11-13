#pragma once

#include "core/system/Config.hpp"
#include "core/system/CommandLineArgs.hpp"


namespace sibr {
    /// Arguments for the foveated renderer application.
    // Neither app loads a dataset, so they don't use BasicIBRAppArgs (whose dataset arguments would show up in --help)
    struct VRHTGSViewerAppArgs :
        virtual WindowAppArgs, virtual RenderingArgs {
        RequiredArg<std::string> modelPath = { "model-path", "Model directory" };
        RequiredArg<std::string> modelPathShort = { "m", "Model directory" };
        Arg<int> device = {"device", 0, "CUDA device index"};
        Arg<bool> exportVisibilityMask = { "export-visibility-mask", "Export the visibility mask for each eye as a line loop (for debugging and use in NeRFICG framework)" };
    };

    struct VRHTGSExperimentAppArgs :
        virtual WindowAppArgs, virtual RenderingArgs {
        RequiredArg<std::string> modelPath = { "model-path", "Experiment config prefix (reads <prefix>_<participant-id>.csv)" };
        RequiredArg<std::string> modelPathShort = { "m", "Experiment config prefix (reads <prefix>_<participant-id>.csv)" };
        Arg<std::string> calibrationDir = { "calibration-dir", "", "Directory holding the per-dataset calibrations (default: 'calibrations' next to the experiment config)" };
        Arg<int> device = {"device", 0, "CUDA device index"};
        Arg<int> participantID = { "participant-id", -1, "Participant ID for experiment" };
        Arg<int> startFromCondition = { "start-from-condition", 0, "Condition index to start from (0 = first condition)" };
        Arg<bool> calibrationMode = { "calibration-mode", "Run in calibration mode (allow adjustment of scene scale + position and write the results to the dataset's calibration directory)" };
        Arg<bool> english = { "english", "Display Questionnaire in English (default is German)" };
    };
}

