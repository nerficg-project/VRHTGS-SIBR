#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdio.h>
#include <string>

#include "imgui_internal.h"
#include "yaml-cpp/yaml.h"

#include "core/graphics/Window.hpp"
#include "core/openxr/OpenXRRdrMode.hpp"
#include "core/renderer/DepthRenderer.hpp"
#include "core/system/String.hpp"
#include "core/view/MultiViewManager.hpp"
#include "core/view/SceneDebugView.hpp"
#include "projects/vrhtgs_viewer/renderer/Config.hpp"
#include "projects/vrhtgs_viewer/renderer/GaussianView.hpp"


constexpr const char* PROGRAM_NAME = "VR-HTGS Viewer";


int main(int argc, char** argv) {
    // Parse Command-line Args
    sibr::CommandLineArgs::parseMainArgs(argc, argv);
    sibr::VRHTGSViewerAppArgs myArgs;
    myArgs.displayHelpIfRequired();
    if (!myArgs.modelPath.isInit() && myArgs.modelPathShort.isInit()) {
        myArgs.modelPath = myArgs.modelPathShort.get();
    }
    int device = myArgs.device;

    // Rendering size
    uint32_t renderingWidth = myArgs.rendering_size.get()[0];
    uint32_t renderingHeight = myArgs.rendering_size.get()[1];
    // Window size
    uint32_t windowWidth = renderingWidth;
    uint32_t windowHeight = renderingHeight;

    // Window setup
    sibr::Window window(PROGRAM_NAME, sibr::Vector2i(50, 50), myArgs,
            sibr::getResourcesDirectory() + "/gaussians/" + PROGRAM_NAME + ".ini");

    // Load model and scene
    const char* modelPath = myArgs.modelPath.get().c_str();
    std::ifstream cfgFile(myArgs.modelPath.get() + "/training_config.yaml");
    if (!cfgFile.good()) {
        SIBR_ERR << "Could not find config file 'training_config.yaml' at " << myArgs.modelPath.get();
    }
    YAML::Node config = YAML::Load(cfgFile);
    myArgs.dataset_path = config["DATASET"]["PATH"].as<std::string>();

    sibr::BasicIBRScene::SceneOptions myOpts;
    myOpts.renderTargets = false;
    myOpts.mesh = false;
    myOpts.images = false;
    myOpts.cameras = true;
    myOpts.texture = false;

    sibr::BasicIBRScene::Ptr scene;
    try {
        scene.reset(new sibr::BasicIBRScene(myArgs, myOpts));
    } catch (...) {
        SIBR_LOG << "Did not find specified input folder, loading from model path" << std::endl;
        myArgs.dataset_path = myArgs.modelPath.get();
        scene.reset(new sibr::BasicIBRScene(myArgs, myOpts));
    }

    std::filesystem::path plyfile = std::filesystem::path(myArgs.modelPath.get()) / "gaussians.ply";
    if (!std::filesystem::exists(plyfile)) {
        plyfile = std::filesystem::path(myArgs.modelPath.get()) / "final.ply";
        if (!std::filesystem::exists(plyfile)) {
            SIBR_ERR << "Could not find 'gaussians.ply' or 'final.ply' in model path " << myArgs.modelPath.get();
        }
    }

    // Fix rendering aspect ratio if user provided rendering size
    uint32_t sceneWidth = scene->cameras()->inputCameras()[0]->w();
    uint32_t sceneHeight = scene->cameras()->inputCameras()[0]->h();
    float scene_aspect_ratio = sceneWidth * 1.0f / sceneHeight;
    float rendering_aspect_ratio = renderingWidth * 1.0f / renderingHeight;
    renderingWidth = (renderingWidth <= 0) ? std::min(1200U, sceneWidth) : renderingWidth;
    renderingHeight = (renderingHeight <= 0) ? std::min(1200U, sceneWidth) / scene_aspect_ratio : renderingHeight;
    if ((renderingWidth > 0) && !myArgs.force_aspect_ratio ) {
        if (abs(scene_aspect_ratio - rendering_aspect_ratio) > 0.001f) {
            if (sceneWidth > sceneHeight) {
                renderingHeight = renderingWidth / scene_aspect_ratio;
            } else {
                renderingWidth = renderingHeight * scene_aspect_ratio;
            }
        }
    }
    sibr::Vector2u usedResolution(renderingWidth, renderingHeight);
    const unsigned int sceneResWidth = usedResolution.x();
    const unsigned int sceneResHeight = usedResolution.y();

    // Create the ULR view
    sibr::HTGaussianView::Ptr gaussianView(
        new sibr::HTGaussianView(scene, sceneResWidth, sceneResHeight,
            reinterpret_cast<const char*>(plyfile.u8string().c_str()),
            1.0f, device, true /* calibration mode to allow controller inputs for movement */
        ));
    if (myArgs.exportVisibilityMask) {
        gaussianView->exportVisibilityMask = true;
        SIBR_LOG << "Visibility mask export enabled. The visibility mask for each eye will be exported as a line loop in a binary file named 'render_mask_view_X.bin' where X is the eye index (0 for left, 1 for right)."
            << "The files will be saved in the current working directory: " << std::filesystem::current_path() << std::endl;
    }

    // Camera handler for main view
    sibr::InteractiveCameraHandler::Ptr generalCamera(new sibr::InteractiveCameraHandler());
    generalCamera->setup(
        scene->cameras()->inputCameras(),
        sibr::Viewport(0, 0, static_cast<float>(usedResolution.x()), static_cast<float>(usedResolution.y())),
        nullptr
    );

    // Add views to mvm.
    std::shared_ptr<sibr::OpenXRRdrMode> openxrMode = nullptr;
    sibr::MultiViewManager multiViewManager(window, false);
    switch (myArgs.rendering_mode) {
        case 1:
            multiViewManager.renderingMode(sibr::IRenderingMode::Ptr(new sibr::StereoAnaglyphRdrMode()));
            break;
        case 2:
            openxrMode = std::make_shared<sibr::OpenXRRdrMode>(window);
            multiViewManager.renderingMode(sibr::IRenderingMode::Ptr(new sibr::OpenXRRdrMode(window)));
            break;
        default:
            break;
    }

    multiViewManager.addIBRSubView("Point view", gaussianView, usedResolution, ImGuiWindowFlags_NoBringToFrontOnFocus);
    multiViewManager.addCameraForView("Point view", generalCamera);

    // Main loop
    while (window.isOpened()) {
        sibr::Input::poll();
        window.makeContextCurrent();
        if (sibr::Input::global().key().isPressed(sibr::Key::Escape)) {
            window.close();
        }
        if (openxrMode) {
            // Poll controller inputs
            auto controller = openxrMode->getControllerState();
            gaussianView->handleCalibrationInput(controller);
        }

        multiViewManager.onUpdate(sibr::Input::global());
        multiViewManager.onRender(window);

        window.swapBuffer();
        CHECK_GL_ERROR;
    }

    return EXIT_SUCCESS;
}
