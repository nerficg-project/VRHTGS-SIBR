#pragma once

#include "core/graphics/GUI.hpp"
#include "core/system/Config.hpp"
#include "core/view/ViewBase.hpp"
#include "projects/vrhtgs_viewer/renderer/GaussianView.hpp"

namespace sibr {
    class VRImguiRenderer;

    struct ExperimentResponses {
        bool english = false;
        bool conditionHidden = false;
        bool showGazeVisualization = false;
        int currentlyAnsweringIndex = 0;

        int ratingArtifactsNoticed = 0; // -3 - +3 scale
        int ratingArtifactsBothered = 0; // -3 - +3 scale
        int ratingVirtualImmersion = 0; // -3 - +3 scale
        int ratingPerceivedPictures = 0; // -3 - +3 scale
        int ratingVirtualPresence = 0; // -3 - +3 scale
        int ratingVirtualCaptivation = 0; // -3 - +3 scale
        int ratingVirtualRealness = 0; // -3 - +3 scale
    };

    class ExperimentView : public sibr::HTGaussianView {
        SIBR_CLASS_PTR(ExperimentView);

    public:
        ExperimentView(uint32_t renderW, uint32_t renderH,
                       const char* file, float scale = 1.0f, int device = 0, bool calibrationMode = false);

        virtual ~ExperimentView() override;

        /**
         * Hide the model and switch to the experiment GUI view
         */
        void showGUI() { this->guiVisible = true; };

        /**
         * Hide the experiment GUI and switch back to the normal model view
         */
        void showScene() { this->guiVisible = false; };

        /**
         * Renders the VR Experiment GUI to an intermediate texture and collects user responses
         */
        void prepareUI(const sibr::Matrix4f& headPose, ExperimentResponses& userResponse);

        /**
         * Perform rendering. Called by the view manager or rendering mode.
         * \param dst The destination rendertarget.
         * \param eye The novel viewpoint.
         */
        void onRenderIBR(sibr::IRenderTarget& dst, const sibr::Camera& eye) override;

        /**
         * Renders a copy of the experiment GUI for the experimenter on screen,
         * allowing manual intervention and monitoring
         * @return true if the submit button is pressed
         */
        bool renderExperimenterGui(ExperimentResponses& userResponse, bool calibrationMode);

    protected:
        static constexpr const int QUESTIONNAIRE_WIDTH = 1280 / 2;
        static constexpr const int QUESTIONNAIRE_HEIGHT = 720 / 2;

        bool guiVisible = false;
        VRImguiRenderer* _vrImguiRenderer;

        /** No stereo render while the questionnaire is shown (onRenderIBR draws the GUI instead). */
        bool stereoApplies() const override { return !guiVisible && HTGaussianView::stereoApplies(); }

        sibr::Matrix4f headPose = sibr::Matrix4f::Identity();  // <- Used for placing the GUI window in VR
    };
}
