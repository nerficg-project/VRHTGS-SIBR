#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdio.h>
#include <string>

#include "imgui_internal.h"

#include "core/graphics/Window.hpp"
#include "core/openxr/OpenXRRdrMode.hpp"
#include "core/renderer/DepthRenderer.hpp"
#include "core/system/String.hpp"
#include "core/view/MultiViewManager.hpp"
#include "core/view/SceneDebugView.hpp"
#include "projects/vrhtgs_viewer/renderer/Config.hpp"
#include "projects/vrhtgs_viewer/renderer/GaussianView.hpp"
#include "projects/vrhtgs_viewer/renderer/ModelConfig.hpp"


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

    // Desktop rendering size (the headset's swapchain size is used in OpenXR mode)
    const sibr::Vector2u usedResolution = sibr::desktopResolution(myArgs.rendering_size.get());

    // Window setup
    sibr::Window window(PROGRAM_NAME, sibr::Vector2i(50, 50), myArgs,
            sibr::getResourcesDirectory() + "/gaussians/" + PROGRAM_NAME + ".ini");

    // Near/far planes from the model's training config, if it has them (no dataset is loaded)
    const sibr::ModelConfig modelConfig = sibr::loadModelConfig(myArgs.modelPath.get());
    const float znear = modelConfig.nearPlane.value_or(sibr::DEFAULT_NEAR_PLANE);
    const float zfar = modelConfig.farPlane.value_or(sibr::DEFAULT_FAR_PLANE);
    SIBR_LOG << "Near/far planes: " << znear << " / " << zfar
             << (modelConfig.nearPlane || modelConfig.farPlane ? " (from training_config.yaml)" : " (defaults)") << std::endl;

    std::filesystem::path plyfile = std::filesystem::path(myArgs.modelPath.get()) / "gaussians.ply";
    if (!std::filesystem::exists(plyfile)) {
        plyfile = std::filesystem::path(myArgs.modelPath.get()) / "final.ply";
        if (!std::filesystem::exists(plyfile)) {
            SIBR_ERR << "Could not find 'gaussians.ply' or 'final.ply' in model path " << myArgs.modelPath.get();
        }
    }

    // Create the view
    sibr::HTGaussianView::Ptr gaussianView(
        new sibr::HTGaussianView(usedResolution.x(), usedResolution.y(),
            reinterpret_cast<const char*>(plyfile.u8string().c_str()),
            1.0f, device, false
        ));
    if (myArgs.exportVisibilityMask) {
        gaussianView->exportVisibilityMask = true;
        SIBR_LOG << "Visibility mask export enabled. The visibility mask for each eye will be exported as a line loop in a binary file named 'render_mask_view_X.bin' where X is the eye index (0 for left, 1 for right)."
            << "The files will be saved in the current working directory: " << std::filesystem::current_path() << std::endl;
    }

    // Camera handler for main view
    sibr::InputCamera startCamera = sibr::makeStartCamera(usedResolution, znear, zfar);
    startCamera.rotation(sibr::Quaternionf(0.0f, 1.0f, 0.0f, 0.0f));
    sibr::InteractiveCameraHandler::Ptr generalCamera(new sibr::InteractiveCameraHandler());
    generalCamera->setup(
        startCamera,
        sibr::Viewport(0, 0, static_cast<float>(usedResolution.x()), static_cast<float>(usedResolution.y())),
        nullptr
    );

    // Desktop navigation as in nerficg's ICGui (travel speed: ICGuiCamera's fixed default, adjustable with +/-)
    generalCamera->switchMode(sibr::InteractiveCameraHandler::ICGUI);

    // Add views to mvm.
    std::shared_ptr<sibr::OpenXRRdrMode> openxrMode = nullptr;
    sibr::MultiViewManager multiViewManager(window, false);
    switch (myArgs.rendering_mode) {
        case 1:
            multiViewManager.renderingMode(sibr::IRenderingMode::Ptr(new sibr::StereoAnaglyphRdrMode()));
            break;
        case 2:
            openxrMode = std::make_shared<sibr::OpenXRRdrMode>(window);
            multiViewManager.renderingMode(openxrMode);
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
            // Head-relative locomotion with the thumbsticks
            auto controller = openxrMode->getControllerState();
            gaussianView->handleLocomotionInput(controller);
        }

        multiViewManager.onUpdate(sibr::Input::global());
        multiViewManager.onRender(window);

        window.swapBuffer();
        CHECK_GL_ERROR;
    }

    return EXIT_SUCCESS;
}
