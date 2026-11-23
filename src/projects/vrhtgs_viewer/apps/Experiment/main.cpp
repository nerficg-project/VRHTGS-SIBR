#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <stdio.h>
#include <string>

#include "imgui_internal.h"

#include "core/graphics/Window.hpp"
#include "core/openxr/OpenXRRdrMode.hpp"
#include "core/raycaster/Raycaster.hpp"
#include "core/renderer/DepthRenderer.hpp"
#include "core/system/String.hpp"
#include "core/view/MultiViewManager.hpp"
#include "core/view/SceneDebugView.hpp"
#include "projects/vrhtgs_viewer/renderer/Config.hpp"
#include "projects/vrhtgs_viewer/renderer/ExperimentView.hpp"
#include "projects/vrhtgs_viewer/renderer/ModelConfig.hpp"


constexpr const char* PROGRAM_NAME = "VR-HTGS Experiment";

/**
 * Struct to hold condition data loaded from experiment file
 */
struct Condition {
    sibr::RasterizerVariant modelType;
    std::filesystem::path modelPath;
    bool antiAliasing;
    bool blurPeriphery;
    sibr::ModelConfig modelConfig;
    std::filesystem::path calibrationDir;
};


static bool hasCalibration(const std::filesystem::path& calibrationDir) {
    return std::filesystem::exists(calibrationDir / "calibration.txt")
        && std::filesystem::exists(calibrationDir / "default_pose.txt");
}


static void logCondition(std::ofstream& logFile, int conditionIndex, const std::tm& condition_tm, const Condition& condition) {
    logFile << "\nCondition " << conditionIndex << " started at: "
            << std::put_time(&condition_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
    logFile << "  Model Type: " << (condition.modelType == sibr::RasterizerVariant::HTGS_FOVEATED ? "VRHTGS" : "HTGS") << std::endl;
    logFile << "  Model Path: " << condition.modelPath << std::endl;
    logFile << "  Anti-Aliasing: " << (condition.antiAliasing ? "True" : "False") << std::endl;
    logFile << "  Blur Periphery: " << (condition.blurPeriphery ? "True" : "False") << std::endl;
    logFile << "  Calibration: " << condition.calibrationDir
            << (hasCalibration(condition.calibrationDir) ? "" : " (missing)") << std::endl;
    logFile.flush();
}


/**
 * The mode a condition's scene renders with: stereo (both eyes in one VR-HTGS call) only for the VRHTGS
 * rasterizer with stereo enabled. The gaze-dot check and the questionnaire always render mono.
 */
static std::string conditionRenderingMode(bool stereoRendering, sibr::RasterizerVariant modelType) {
    if (modelType != sibr::RasterizerVariant::HTGS_FOVEATED) {
        return "Mono (the HTGS rasterizer has no stereo path)";
    }
    return stereoRendering ? "Stereo" : "Mono (stereo disabled)";
}


static void logUserResponses(std::ofstream& logFile, const sibr::ExperimentResponses& responses) {
    logFile << "  User Responses:" << std::endl;
    logFile << "    Artifacts Noticed: " << responses.ratingArtifactsNoticed << std::endl;
    logFile << "    Artifacts Bothered: " << responses.ratingArtifactsBothered << std::endl;
    logFile << "    Virtual Immersion: " << responses.ratingVirtualImmersion << std::endl;
    logFile << "    Perceived Pictures: " << responses.ratingPerceivedPictures << std::endl;
    logFile << "    Virtual Presence: " << responses.ratingVirtualPresence << std::endl;
    logFile << "    Virtual Captivation: " << responses.ratingVirtualCaptivation << std::endl;
    logFile << "    Virtual Realness: " << responses.ratingVirtualRealness << std::endl;
    logFile.flush();
}


int main(int argc, char** argv) {
    // Parse Command-line Args
    sibr::CommandLineArgs::parseMainArgs(argc, argv);
    sibr::VRHTGSExperimentAppArgs myArgs;
    myArgs.displayHelpIfRequired();
    if (!myArgs.modelPath.isInit() && myArgs.modelPathShort.isInit()) {
        myArgs.modelPath = myArgs.modelPathShort.get();
    }
    int device = myArgs.device;
    int startFromCondition = myArgs.startFromCondition;
    bool calibrationMode = myArgs.calibrationMode;

    // Desktop rendering size (the headset's swapchain size is used for the participant's view)
    const sibr::Vector2u usedResolution = sibr::desktopResolution(myArgs.rendering_size.get());

    // Calibrations are stored per dataset, by default in 'calibrations' next to the experiment config
    // (nerficg's vrhtgs_create_experiment_config.py writes all configs into one shared folder)
    const std::filesystem::path calibrationRoot = myArgs.calibrationDir.get().empty()
        ? std::filesystem::path(myArgs.modelPath.get()).parent_path() / "calibrations"
        : std::filesystem::path(myArgs.calibrationDir.get());

    // Window setup
    sibr::Window window(PROGRAM_NAME, sibr::Vector2i(50, 50), myArgs,
            sibr::getResourcesDirectory() + "/gaussians/" + PROGRAM_NAME + ".ini");

    // Set up MultiViewManager
    std::shared_ptr<sibr::OpenXRRdrMode> openxrMode = nullptr;
    sibr::MultiViewManager multiViewManager(window, false);
    switch (myArgs.rendering_mode) {
        case 2:
            openxrMode = std::make_shared<sibr::OpenXRRdrMode>(window);
            multiViewManager.renderingMode(openxrMode);
            break;
        default:
            SIBR_ERR << "Only rendering mode 2 (OpenXR) is supported in this experiment." << std::endl;
            return -1;
    }

    // Load experiment configuration
    std::ifstream cfgFile(myArgs.modelPath.get() + "_" + std::to_string(myArgs.participantID.get()) +  ".csv");
    if (!cfgFile.good())
    {
        SIBR_ERR << "Could not find config file '" << myArgs.modelPath.get() << "'." << std::endl;
    }

    // Read experiment config as CSV with columns MODEL_TYPE,MODEL_PATH,ANTI_ALIASING,BLUR_PERIPHERY
    // (as written by nerficg/scripts/vrhtgs_create_experiment_config.py), MODEL_TYPE being HTGS or VRHTGS
    std::string line;
    std::vector<Condition> conditions;

    // Read experiment conditions
    while (std::getline(cfgFile, line)) {
        // Configs written on Windows use CRLF line endings
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::stringstream ss(line);
        std::string item;
        std::vector<std::string> tokens;

        // Split by comma
        while (std::getline(ss, item, ',')) {
            tokens.push_back(item);
        }

        if (tokens.size() == 0) {
            continue; // skip empty lines
        } else if (tokens.size() != 4) {
            SIBR_ERR << "Invalid config line: '" << line << "'. Expected 4 tokens, got " << tokens.size() << "." << std::endl;
            continue;
        }

        Condition cond;
        if (tokens[0] == "VRHTGS") {
            cond.modelType = sibr::RasterizerVariant::HTGS_FOVEATED;
        } else if (tokens[0] == "HTGS") {
            cond.modelType = sibr::RasterizerVariant::HTGS;
        } else {
            SIBR_ERR << "Invalid model type '" << tokens[0] << "' in config line: '" << line << "'. Expected HTGS or VRHTGS." << std::endl;
            continue;
        }
        cond.modelPath = tokens[1];
        if (!std::filesystem::is_directory(cond.modelPath)) {
            SIBR_ERR << "Invalid model path: '" << cond.modelPath << "'." << std::endl;
        }
        cond.antiAliasing = (tokens[2] == "True") ? true : false;
        cond.blurPeriphery = (tokens[3] == "True") ? true : false;

        cond.modelConfig = sibr::loadModelConfig(cond.modelPath);
        std::filesystem::path datasetName = cond.modelConfig.datasetName();
        if (datasetName.empty()) {
            datasetName = cond.modelPath.filename();
            SIBR_WRG << (cond.modelConfig.found ? "No DATASET.PATH in the training_config.yaml of "
                                                : "No usable training_config.yaml in ")
                     << cond.modelPath << ", so its dataset is unknown. Storing its calibration under the model name "
                     << datasetName << " instead." << std::endl;
        }
        cond.calibrationDir = calibrationRoot / datasetName;

        conditions.push_back(cond);
    }

    if (startFromCondition > 0 && startFromCondition < static_cast<int>(conditions.size())) {
        conditions.erase(conditions.begin(), conditions.begin() + startFromCondition);
        SIBR_LOG << "Starting from condition index " << startFromCondition << std::endl;
    }

    if (conditions.size() == 0) {
        SIBR_ERR << "No valid experiment conditions found in config file." << std::endl;
        return -1;
    }

    // Warn about datasets without a calibration: they would run at scale 1 with an identity pose, which is easy to miss
    std::vector<std::filesystem::path> missingCalibrations;
    for (const auto& condition : conditions) {
        if (!hasCalibration(condition.calibrationDir)
            && std::find(missingCalibrations.begin(), missingCalibrations.end(), condition.calibrationDir) == missingCalibrations.end()) {
            missingCalibrations.push_back(condition.calibrationDir);
        }
    }
    for (const auto& dir : missingCalibrations) {
        if (calibrationMode) {
            SIBR_LOG << "No calibration yet in " << dir << ", it will be created." << std::endl;
        } else {
            SIBR_WRG << "No calibration found in " << dir << " (calibration.txt / default_pose.txt)."
                     << " The scene will use scale 1 and the identity pose. Run with --calibration-mode first." << std::endl;
        }
    }

    // Set up logging to file (only now, so runs rejected during setup don't leave empty logs behind)
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_s(&tm_buf, &time_t);

    std::ostringstream log_filename;
    log_filename << "experiment_log_participant_" << myArgs.participantID.get() << "_"
                 << std::put_time(&tm_buf, "%Y%m%d_%H%M%S")
                 << ".txt";
    std::ostringstream recording_filename;
    recording_filename << "experiment_recording_participant_" << myArgs.participantID.get()
                    << "_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S")
                    << ".htgsrec";

    std::ofstream logFile(log_filename.str(), std::ios::out | std::ios::app);
    if (!logFile.is_open()) {
        SIBR_ERR << "Could not open log file: " << log_filename.str() << std::endl;
        return -1;
    }

    logFile << "Experiment Log for Participant " << myArgs.participantID.get() << " - Started at: "
            << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << std::endl;
    logFile << "========================================" << std::endl;
    logFile << (myArgs.noStereo
                ? "Stereo Rendering: disabled (--no-stereo), all conditions render mono"
                : "Stereo Rendering: enabled, VRHTGS conditions render in stereo, HTGS conditions in mono")
            << std::endl;
    if (!calibrationMode) {
        for (const auto& dir : missingCalibrations) {
            logFile << "WARNING: No calibration found in " << dir << ", using scale 1 and the identity pose." << std::endl;
        }
    }
    if (myArgs.noRecording) {
        logFile << "Recording disabled (--no-recording), no .htgsrec file is written." << std::endl;
    }
    logFile.flush();

    // Set up view
    sibr::ExperimentView::Ptr gaussianView = nullptr;
    sibr::InteractiveCameraHandler::Ptr generalCamera(new sibr::InteractiveCameraHandler());

    // Start experiment loop
    int conditionIndex = 0;
    int completedConditions = 0;  // conditionIndex also counts a condition that was aborted
    bool eyeTrackingShown = false;
    bool english = myArgs.english;  // The experimenter GUI can switch the language; keep it across conditions
    for (const auto& condition : conditions) {
        conditionIndex++;
        SIBR_LOG << "Starting condition with model: " << condition.modelPath << std::endl;

        // Log condition start
        const auto condition_start = std::chrono::system_clock::now();
        const auto condition_time = std::chrono::system_clock::to_time_t(condition_start);
        std::tm condition_tm;
        localtime_s(&condition_tm, &condition_time);
        logCondition(logFile, conditionIndex, condition_tm, condition);

        const std::filesystem::path plyPath = sibr::findModelPly(condition.modelPath);
        if (plyPath.empty()) {
            SIBR_ERR << "Could not find 'gaussians.ply', 'final.ply' or another loadable .ply in model path " << condition.modelPath << std::endl;
        }
        std::string plyfile = plyPath.string();

        // Load the dataset's calibration (missing calibrations were already warned about at startup)
        const std::filesystem::path calibPath = condition.calibrationDir / "calibration.txt";
        const std::filesystem::path posePath = condition.calibrationDir / "default_pose.txt";
        std::ifstream calibFile(calibPath);
        float model_scale = 1.0f;
        if (calibFile.good()) {
            SIBR_LOG << "Loading calibration from " << calibPath << "." << std::endl;
            calibFile >> model_scale;
        }

        if (!gaussianView) {
            // Create the view
            gaussianView.reset(
                new sibr::ExperimentView(usedResolution.x(), usedResolution.y(), plyfile.c_str(), model_scale, device, calibrationMode)
            );
            // Stereo is on by default; it only applies to VRHTGS conditions (HTGS always renders mono)
            gaussianView->stereoRendering = !myArgs.noStereo;
            SIBR_LOG << "Stereo rendering: " << (gaussianView->stereoRendering
                ? "on for VRHTGS conditions (HTGS conditions render mono; disable with --no-stereo)"
                : "off (--no-stereo)") << std::endl;
            // Without enableRecording no record is written in either path (mono onRenderIBR or the
            // stereo hook's mask records), so --no-recording covers stereo too
            if (myArgs.noRecording) {
                SIBR_LOG << "Recording disabled (--no-recording)." << std::endl;
            } else {
                gaussianView->enableRecording(recording_filename.str());
            }
            openxrMode->setFrameBeginCallback(
                [view = std::weak_ptr<sibr::ExperimentView>(gaussianView)](int64_t displayTime, int64_t displayPeriod) {
                    if (auto v = view.lock()) {
                        v->beginXRFrame(displayTime, displayPeriod);
                    }
                });
            multiViewManager.addIBRSubView("Point view", gaussianView, usedResolution, ImGuiWindowFlags_NoBringToFrontOnFocus);
            multiViewManager.addCameraForView("Point view", generalCamera);
        } else {
            gaussianView->loadModel(plyfile.c_str(), model_scale);
        }

        // Update experiment condition parameters
        gaussianView->antiAliasing = condition.antiAliasing;
        gaussianView->blurPeriphery = condition.blurPeriphery;
        gaussianView->modelType = condition.modelType;
        gaussianView->showScene();
        gaussianView->setConditionInfo(conditionIndex, conditions.size(), condition.modelType, condition.modelPath.string(), condition.antiAliasing);

        // The mode this condition's scene renders with. The experimenter GUI can still switch stereo or the
        // rasterizer at runtime, so changes are logged as they happen (checked once per loop iteration).
        std::string renderingMode = conditionRenderingMode(gaussianView->stereoRendering, gaussianView->modelType);
        logFile << "  Rendering Mode: " << renderingMode << std::endl;
        logFile.flush();
        SIBR_LOG << "Rendering mode: " << renderingMode << std::endl;

        // The near plane is fixed per condition (AAA Gaussians mode supports a closer one), independent of the
        // model's training config, so all conditions of the study are comparable. Only the far plane comes from it.
        const float znear = condition.antiAliasing ? 0.02f : sibr::DEFAULT_NEAR_PLANE;
        const float zfar = condition.modelConfig.farPlane.value_or(sibr::DEFAULT_FAR_PLANE);
        SIBR_LOG << "Near/far planes: " << znear << " / " << zfar << std::endl;

        sibr::Matrix4f scene_transform = sibr::Matrix4f::Identity();
        std::ifstream poseFile(posePath);
        if (poseFile.good()) {
            sibr::Matrix4f transform = sibr::Matrix4f::Identity();
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    poseFile >> transform(i, j);
                }
            }
            scene_transform = transform.inverse();
        }
        gaussianView->setSceneTransform(scene_transform);

        // Reset the camera to the origin (the calibrated scene transform places the scene around it)
        generalCamera->setup(
            sibr::makeStartCamera(usedResolution, znear, zfar),
            sibr::Viewport(0, 0, static_cast<float>(usedResolution.x()), static_cast<float>(usedResolution.y())),
            nullptr
        );

        sibr::ExperimentResponses userResponseData;
        userResponseData.english = english;
        if (!calibrationMode && !eyeTrackingShown) {
            std::cout << "Showing eye tracking visualization for the first time. Press A or X on the controller to hide it." << std::endl;
            gaussianView->showScene();  // GUI takes precedence over the gaze visualization, so make sure it is hidden
            gaussianView->showGazeVisualization();
            userResponseData.showGazeVisualization = true;
            eyeTrackingShown = true;
        }

        // Main loop
        while (window.isOpened()) {
            if (userResponseData.conditionHidden) {
                gaussianView->prepareUI(openxrMode->getHeadPose(), userResponseData);
            }
            window.makeImguiContextCurrent();

            sibr::Input::poll();
            window.makeContextCurrent();
            if (sibr::Input::global().key().isPressed(sibr::Key::Escape)) {
                logFile << "  User aborted experiment at condition " << conditionIndex << std::endl;
                logFile.flush();
                window.close();
            }

            // Render experiment GUI to experimenter screen
            bool condition_complete = gaussianView->renderExperimenterGui(userResponseData, calibrationMode);
            english = userResponseData.english;

            // Poll controller inputs
            auto controller = openxrMode->getControllerState();

            if (!calibrationMode) {
                if (userResponseData.showGazeVisualization) {
                    userResponseData.showGazeVisualization = !controller.confirmState;
                } else if (!userResponseData.conditionHidden) {
                    userResponseData.conditionHidden = controller.confirmState;
                } else {
                    int* current_response = nullptr;
                    switch (userResponseData.currentlyAnsweringIndex) {
                        case 0: current_response = &userResponseData.ratingArtifactsNoticed; break;
                        case 1: current_response = &userResponseData.ratingArtifactsBothered; break;
                        case 2: current_response = &userResponseData.ratingVirtualImmersion; break;
                        case 3: current_response = &userResponseData.ratingPerceivedPictures; break;
                        case 4: current_response = &userResponseData.ratingVirtualPresence; break;
                        case 5: current_response = &userResponseData.ratingVirtualCaptivation; break;
                        case 6: current_response = &userResponseData.ratingVirtualRealness; break;
                        default: break;
                    }

                    // Navigate questionnaire
                    if (controller.confirmState) {
                        userResponseData.currentlyAnsweringIndex++;
                        if (userResponseData.currentlyAnsweringIndex > 6) {
                            userResponseData.currentlyAnsweringIndex = 0;
                            condition_complete = true;
                        }
                    } else if (controller.backState) {
                        userResponseData.currentlyAnsweringIndex--;
                        if (userResponseData.currentlyAnsweringIndex < 0) {
                            userResponseData.currentlyAnsweringIndex = 0;
                        }
                    } else if (controller.leftPressed && current_response) {
                        (*current_response)--;
                        if (*current_response < -3) {
                            *current_response = -3;
                        }
                    } else if (controller.rightPressed && current_response) {
                        (*current_response)++;
                        if (*current_response > 3) {
                            *current_response = 3;
                        }
                    }
                }
            } else {
                if (controller.confirmState) {
                    condition_complete = true;
                } else {
                    // Allow model manipulation in calibration mode
                    gaussianView->handleCalibrationInput(controller);
                }
            }

            if (condition_complete) {
                // Record user response and timing
                auto condition_end = std::chrono::system_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(condition_end - condition_start);

                completedConditions++;
                logFile << "  User completed condition" << std::endl;
                logFile << "  Duration: " << duration.count() << " ms" << std::endl;
                logUserResponses(logFile, userResponseData);
                logFile.flush();

                gaussianView->showGUI();
                multiViewManager.onUpdate(sibr::Input::global());
                multiViewManager.onRender(window);
                window.swapBuffer();
                CHECK_GL_ERROR;
                break; // Next condition
            }

            if (userResponseData.showGazeVisualization) {
                gaussianView->showScene();
                gaussianView->showGazeVisualization();
            } else if (userResponseData.conditionHidden) {
                gaussianView->showGUI();
                gaussianView->showGazeVisualization();
            } else {
                gaussianView->showScene();
                gaussianView->hideGazeVisualization();
            }

            const std::string currentRenderingMode = conditionRenderingMode(gaussianView->stereoRendering, gaussianView->modelType);
            if (currentRenderingMode != renderingMode) {
                renderingMode = currentRenderingMode;
                logFile << "  Rendering Mode changed in the experimenter GUI to: " << renderingMode << std::endl;
                logFile.flush();
                SIBR_LOG << "Rendering mode changed to: " << renderingMode << std::endl;
            }

            multiViewManager.onUpdate(sibr::Input::global());
            multiViewManager.onRender(window);
            window.swapBuffer();
            CHECK_GL_ERROR;
        }

        if (!window.isOpened()) {
            break;
        }

        if (calibrationMode) {
            // Store calibrated scene transform and model scale
            sibr::Matrix4f finalTransform = gaussianView->getSceneTransform().inverse();
            float finalModelScale = gaussianView->getCalibratedModelScale();

            std::filesystem::create_directories(condition.calibrationDir);
            std::ofstream calibOut(condition.calibrationDir / "calibration.txt");
            if (calibOut.good()) {
                calibOut << finalModelScale << std::endl;
                SIBR_LOG << "Saved calibrated model scale: " << finalModelScale << std::endl;
            }
            std::ofstream poseOut(condition.calibrationDir / "default_pose.txt");
            if (poseOut.good()) {
                poseOut << finalTransform << std::endl;
                SIBR_LOG << "Saved calibrated default pose to " << (condition.calibrationDir / "default_pose.txt") << "." << std::endl;
            } else {
                SIBR_WRG << "Could not write the calibration to " << condition.calibrationDir << "." << std::endl;
            }
        }
    }

    if (window.isOpened()) {
        window.close();
    }

    // Write experiment completion to log
    auto end_time = std::chrono::system_clock::now();
    auto end_time_t = std::chrono::system_clock::to_time_t(end_time);
    std::tm end_tm;
    localtime_s(&end_tm, &end_time_t);

    logFile << "\n========================================" << std::endl;
    logFile << "Experiment ended at: " << std::put_time(&end_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
    logFile << "Total conditions completed: " << completedConditions << "/" << conditions.size() << std::endl;
    logFile.close();

    SIBR_LOG << "Experiment log saved to: " << log_filename.str() << std::endl;

    return EXIT_SUCCESS;
}
