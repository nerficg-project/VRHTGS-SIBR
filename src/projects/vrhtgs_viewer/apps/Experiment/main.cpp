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
#include "yaml-cpp/yaml.h"

#include "core/graphics/Window.hpp"
#include "core/openxr/OpenXRRdrMode.hpp"
#include "core/raycaster/Raycaster.hpp"
#include "core/renderer/DepthRenderer.hpp"
#include "core/system/String.hpp"
#include "core/view/MultiViewManager.hpp"
#include "core/view/SceneDebugView.hpp"
#include "projects/vrhtgs_viewer/renderer/Config.hpp"
#include "projects/vrhtgs_viewer/renderer/ExperimentView.hpp"


constexpr const char* PROGRAM_NAME = "VR-HTGS Experiment";

/**
 * Struct to hold condition data loaded from experiment file
 */
struct Condition {
    sibr::RasterizerVariant modelType;
    std::filesystem::path modelPath;
    bool antiAliasing;
    bool blurPeriphery;
};


static void logCondition(std::ofstream& logFile, int conditionIndex, const std::tm& condition_tm, const Condition& condition) {
    logFile << "\nCondition " << conditionIndex << " started at: "
            << std::put_time(&condition_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
    logFile << "  Model Type: " << (condition.modelType == sibr::RasterizerVariant::HTGS_FOVEATED ? "HTGS_Foveated" : "HTGS") << std::endl;
    logFile << "  Model Path: " << condition.modelPath << std::endl;
    logFile << "  Anti-Aliasing: " << (condition.antiAliasing ? "True" : "False") << std::endl;
    logFile << "  Blur Periphery: " << (condition.blurPeriphery ? "True" : "False") << std::endl;
    logFile << "  Periphery Mode: " << "linear" << std::endl;
    logFile.flush();
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

    // Rendering size
    uint32_t rendering_width = myArgs.rendering_size.get()[0];
    uint32_t rendering_height = myArgs.rendering_size.get()[1];
    // Window size
    uint32_t windowWidth = rendering_width; // myArgs.win_width;
    uint32_t windowHeight = rendering_height; // myArgs.windowHeight;

    // Window setup
    sibr::Window window(PROGRAM_NAME, sibr::Vector2i(50, 50), myArgs,
            sibr::getResourcesDirectory() + "/gaussians/" + PROGRAM_NAME + ".ini");

    // Set up logging to file
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
    logFile.flush();

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

    // Read experiment config as CSV with columns MODEL_TYPE,MODEL_PATH,ANTI_ALIASING,BLUR_PERIPEHRY,PERIPHERY_INTERPOLATION_MODE
    std::string line;
    std::vector<Condition> conditions;

    // Read experiment conditions
    while (std::getline(cfgFile, line)) {
        std::stringstream ss(line);
        std::string item;
        std::vector<std::string> tokens;

        // Split by comma
        while (std::getline(ss, item, ',')) {
            tokens.push_back(item);
        }

        if (tokens.size() == 0) {
            continue; // skip empty lines
        } else if (tokens.size() != 5) {
            SIBR_ERR << "Invalid config line: '" << line << "'. Expected 5 tokens, got " << tokens.size() << "." << std::endl;
            continue;
        }

        Condition cond;
        cond.modelType = (tokens[0] == "HTGS_Foveated") ? sibr::RasterizerVariant::HTGS_FOVEATED : sibr::RasterizerVariant::HTGS;
        cond.modelPath = tokens[1];
        if (!std::filesystem::is_directory(cond.modelPath)) {
            SIBR_ERR << "Invalid model path: '" << cond.modelPath << "'." << std::endl;
        }
        cond.antiAliasing = (tokens[2] == "True") ? true : false;
        cond.blurPeriphery = (tokens[3] == "True") ? true : false;

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

    // Set up options
    sibr::BasicIBRScene::SceneOptions myOpts;
    myOpts.renderTargets = false;
    myOpts.mesh = true;
    myOpts.images = false;
    myOpts.cameras = true;
    myOpts.texture = false;
    const uint32_t flags = SIBR_GPU_LINEAR_SAMPLING | SIBR_FLIP_TEXTURE;

    // Set up scene
    sibr::BasicIBRScene::Ptr scene = nullptr;
    sibr::ExperimentView::Ptr gaussianView = nullptr;
    sibr::InteractiveCameraHandler::Ptr generalCamera(new sibr::InteractiveCameraHandler());

    // Start experiment loop
    int conditionIndex = 0;
    bool eyeTrackingShown = false;
    for (const auto& condition : conditions) {
        conditionIndex++;
        SIBR_LOG << "Starting condition with model: " << condition.modelPath << std::endl;

        // Log condition start
        const auto condition_start = std::chrono::system_clock::now();
        const auto condition_time = std::chrono::system_clock::to_time_t(condition_start);
        std::tm condition_tm;
        localtime_s(&condition_tm, &condition_time);
        logCondition(logFile, conditionIndex, condition_tm, condition);

        // Parse and load next scene
        std::ifstream conditionCfg(condition.modelPath / "training_config.yaml");
        if (!conditionCfg.good())
        {
            SIBR_ERR << "Could not find config file 'training_config.yaml' at " << condition.modelPath;
        }

        YAML::Node config = YAML::Load(conditionCfg);
        auto datasetPath = std::filesystem::path(config["DATASET"]["PATH"].as<std::string>());
        if (!datasetPath.is_absolute()) {
            datasetPath = std::filesystem::path(myArgs.modelPath.get()).parent_path().parent_path() / datasetPath;
        }
        myArgs.dataset_path = datasetPath.string();

        try {
            scene.reset(new sibr::BasicIBRScene(myArgs, myOpts));
        } catch (...) {
            SIBR_LOG << "Did not find specified input folder, loading from model path" << std::endl;
            myArgs.dataset_path = myArgs.modelPath.get();
            scene.reset(new sibr::BasicIBRScene(myArgs, myOpts));
        }

        std::string plyfile = (condition.modelPath / "gaussians.ply").string();

        // Fix rendering aspect ratio if user provided rendering size
        uint32_t scene_width = scene->cameras()->inputCameras()[0]->w();
        uint32_t scene_height = scene->cameras()->inputCameras()[0]->h();
        float scene_aspect_ratio = scene_width * 1.0f / scene_height;
        float rendering_aspect_ratio = rendering_width * 1.0f / rendering_height;
        rendering_width = (rendering_width <= 0) ? std::min(1200U, scene_width) : rendering_width;
        rendering_height = (rendering_height <= 0) ? std::min(1200U, scene_width) / scene_aspect_ratio : rendering_height;
        if ((rendering_width > 0) && !myArgs.force_aspect_ratio ) {
            if (abs(scene_aspect_ratio - rendering_aspect_ratio) > 0.001f) {
                if (scene_width > scene_height) {
                    rendering_height = rendering_width / scene_aspect_ratio;
                } else {
                    rendering_width = rendering_height * scene_aspect_ratio;
                }
            }
        }
        sibr::Vector2u usedResolution(rendering_width, rendering_height);
        const unsigned int sceneResWidth = usedResolution.x();
        const unsigned int sceneResHeight = usedResolution.y();

        std::ifstream calibFile(datasetPath / "calibration.txt");
        float model_scale = 1.0f;
        if (calibFile.good()) {
            SIBR_LOG << "Loading calibration from '" << (datasetPath / "calibration.txt") << "'." << std::endl;
            calibFile >> model_scale;
        }

        if (!gaussianView) {
            // Create the ULR view.
            gaussianView.reset(
                new sibr::ExperimentView(scene, sceneResWidth, sceneResHeight, plyfile.c_str(), model_scale, device, calibrationMode)
            );
            gaussianView->enableRecording(recording_filename.str());
            multiViewManager.addIBRSubView("Point view", gaussianView, usedResolution, ImGuiWindowFlags_NoBringToFrontOnFocus);
            multiViewManager.addCameraForView("Point view", generalCamera);
        } else {
            // Update the scene and resolution in existing view
            gaussianView->setScene(scene);
            gaussianView->setResolution(usedResolution.cast<int>());
            gaussianView->loadModel(plyfile.c_str(), model_scale);
        }

        // Update experiment condition parameters
        gaussianView->antiAliasing = condition.antiAliasing;
        gaussianView->blurPeriphery = condition.blurPeriphery;
        gaussianView->modelType = condition.modelType;
        gaussianView->showScene();
        gaussianView->setConditionInfo(conditionIndex, conditions.size(), condition.modelType, condition.modelPath.string(), condition.antiAliasing);

        // Load default camera
        sibr::InputCamera::Ptr defaultCamera;
        sibr::Matrix4f scene_transform = sibr::Matrix4f::Identity();
        defaultCamera.reset(new sibr::InputCamera());
        defaultCamera->position(sibr::Vector3f(0, 0, 0));
        if (condition.antiAliasing) {
            defaultCamera->znear(0.02f);  // AAA Gaussians mode supports closer near plane
        }

        std::ifstream poseFile(datasetPath / "default_pose.txt");
        if (poseFile.good()) {
            sibr::Matrix4f transform = sibr::Matrix4f::Identity();
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    poseFile >> transform(i, j);
                }
            }
            scene_transform = transform.inverse();
        } else {
            SIBR_WRG << "Could not find default camera pose at '" << (datasetPath / "default_pose.txt") << "'." << std::endl;
        }
        gaussianView->setSceneTransform(scene_transform);

        // Reset camera
        std::vector<sibr::InputCamera::Ptr> cams = scene->cameras()->inputCameras();
        cams.insert(cams.begin(), defaultCamera);
        generalCamera->setup(
            cams,
            sibr::Viewport(0, 0, static_cast<float>(usedResolution.x()), static_cast<float>(usedResolution.y())),
            nullptr
        );

        sibr::ExperimentResponses userResponseData;
        userResponseData.english = myArgs.english;
        if (!calibrationMode && !eyeTrackingShown) {
            std::cout << "Showing eye tracking visualization for the first time. Press SPACE to hide it during the experiment." << std::endl;
            gaussianView->showGUI();
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

                logFile << "  User completed condition (Space pressed)" << std::endl;
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
                gaussianView->showGazeVisualization();
            } else if (userResponseData.conditionHidden) {
                gaussianView->showGUI();
                gaussianView->showGazeVisualization();
            } else {
                gaussianView->showScene();
                gaussianView->hideGazeVisualization();
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

            std::ofstream calibOut(datasetPath / "calibration.txt");
            if (calibOut.good()) {
                calibOut << finalModelScale << std::endl;
                SIBR_LOG << "Saved calibrated model scale: " << finalModelScale << std::endl;
            }
            std::ofstream poseOut(datasetPath / "default_pose.txt");
            if (poseOut.good()) {
                poseOut << finalTransform << std::endl;
                SIBR_LOG << "Saved calibrated default pose to '" << (datasetPath / "default_pose.txt") << "'." << std::endl;
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
    logFile << "Total conditions completed: " << conditionIndex << "/" << conditions.size() << std::endl;
    logFile.close();

    SIBR_LOG << "Experiment log saved to: " << log_filename.str() << std::endl;

    return EXIT_SUCCESS;
}
