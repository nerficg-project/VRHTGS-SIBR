#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include <cuda_runtime.h>
#include <cuda_gl_interop.h>
#include <GL/glew.h>

#include "Config.hpp"
#include "core/graphics/Mesh.hpp"
#include "core/graphics/Texture.hpp"
#include "core/renderer/CopyRenderer.hpp"
#include "core/renderer/PointBasedRenderer.hpp"
#include "core/renderer/RenderMaskHolder.hpp"
#include "core/system/SimpleTimer.hpp"
#include "core/system/Config.hpp"
#include "core/view/ViewBase.hpp"
#include "projects/vrhtgs_viewer/renderer/Recording.hpp"
#include "VRHTGSCudaBackend/rasterization/include/config.h"
#include "VRHTGSCudaBackend/rasterization/include/utils/rasterization_utils.h"


namespace sibr {
    // Forward declarations
    class BufferCopyRenderer;
    struct ControllerState;

    // Define the types and sizes that make up the contents of each Gaussian
    //  in the trained model.
    typedef Vector3f Pos;

    struct Scale {
        float scale[3];
    };

    struct Rot {
        float rot[4];
    };

    template<int D>
    struct SHs {
        float shs[(D+1)*(D+1)*3];
    };

    template<int D>
    struct RichPoint {
        Pos pos;
        float n[3];
        SHs<D> shs;
        float opacity;
        Scale scale;
        Rot rot;
    };

    // Enum to specify the currently used rasterizer (HTGS or HTGS_foveated)
    enum class RasterizerVariant : uint8_t {
        HTGS = 0,
        HTGS_FOVEATED = 1,
    };

    // Rasterization helper buffers
    struct RasterizationBuffer {
        std::function<char*(size_t)> resizeFunc = nullptr;
        char* data = nullptr;
        size_t size = 0;

        RasterizationBuffer() = default;
        RasterizationBuffer(const RasterizationBuffer&) = delete;  // resizeFunc captures this buffer's address
        RasterizationBuffer& operator=(const RasterizationBuffer&) = delete;

        void release() {
            if (data) {
                cudaFree(data);
            }
            data = nullptr;
            size = 0;
        }

        ~RasterizationBuffer() { release(); }
    };

    /**
     * GPU state one eye's rasterization needs: pose buffers, rasterizer helper buffers and the GL
     * image buffer (with its CUDA registration) the eye's image ends up in.
     * HTGaussianView keeps two, indexed by eye slot. Mono rendering only uses slot 0; slot 1 is
     * allocated on first stereo use.
     */
    struct EyeGpuState {
        bool allocated = false;
        float* viewCuda = nullptr;
        float* projCuda = nullptr;
        float* invRVPCuda = nullptr;
        float* camPosCuda = nullptr;
        RasterizationBuffer perPrimitiveBuffer, perTileBuffer, perSubtileBuffer, perInstanceBuffer, blurImageBuffer;
        GLuint imageBuffer = 0;
        cudaGraphicsResource_t imageBufferCuda = nullptr;
    };

    /**
     * Everything the rasterizers, the gaze visualization and the recording need for one eye, in the
     * renderer's conventions. Built by HTGaussianView::computeEyeRenderParams, the only place this
     * math lives, so mono and stereo rendering use identical values.
     * The matrices are stored transposed (column-major data = row-major float4 rows), i.e. exactly
     * the layout uploaded to the GPU and written to the recording.
     */
    struct EyeRenderParams {
        uint32_t slot = 0;       // Per-eye array index (0/1; 0 for mono / unexpected view indices)
        uint32_t eyeIndex = 0;   // Raw Camera::viewIndex(), as written to the recording
        float focalX = 0.0f, focalY = 0.0f;
        float centerX = 0.0f, centerY = 0.0f;
        float gazePos[2] = {0.0f, 0.0f};  // Gaze position in pixels
        sibr::Matrix4f view;     // World (scene) -> camera, transposed
        sibr::Matrix4f viewProj; // Projection * view, transposed
        sibr::Matrix4f invRVP;   // Inverse of rotation-only view-projection, transposed
        sibr::Vector3f eyePos;   // Eye position in scene (model) space
        float zNear = 0.0f, zFar = 0.0f;
    };

    /**
     * \class HTGaussianView
     */
    class HTGaussianView : public sibr::ViewBase {
        SIBR_CLASS_PTR(HTGaussianView);

    public:
        /**
         * Constructor
         * \param renderW rendering width
         * \param renderH rendering height
         */
        HTGaussianView(uint32_t renderW, uint32_t renderH,
                const char* file, float scale = 1.0f, int device = 0, bool calibrationMode = false);

        virtual ~HTGaussianView() override;

        /**
         * Load a model from a PLY file with optional background model data in the same parent directory.
         * \param file The PLY file path.
         */
        void loadModel(const char* file, float scale = 1.0f);

        /**
         * Perform rendering. Called by the view manager or rendering mode.
         * \param dst The destination rendertarget.
         * \param eye The novel viewpoint.
         */
        void onRenderIBR(sibr::IRenderTarget& dst, const sibr::Camera& eye) override;

        /**
         * Stereo hook: called once per frame with both eye cameras before the two per-eye
         * onRenderIBR calls. If stereo applies (see stereoApplies()), renders both eyes with one
         * VR-HTGS stereo call and marks both eye images as ready; onRenderIBR then only shows the
         * ready image for its eye. Otherwise does nothing and onRenderIBR renders mono.
         */
        void onPrepareStereoFrame(const sibr::Camera& left, const sibr::Camera& right) override;

        /**
         * Update inputs (do nothing).
         * \param input The inputs state.
         */
        void onUpdate(Input& input) override;

        /**
         * Update the GUI.
         */
        void onGUI() override;

        /**
         * Update the view resolution
         */
        void setResolution(const Vector2i& size) override;

        /**
         * Hide the model rendering and only show a gaze visualization dot
         * Useful for confirming calibration and checking if eye tracking is working
         */
        void showGazeVisualization() { this->gazeVisualization = true; };

        /**
         * Hide gaze visualization and show normal model render
         */
        void hideGazeVisualization() { this->gazeVisualization = false; };

        /**
         * Sets the model transform matrix for the current scene
         */
        void setSceneTransform(const sibr::Matrix4f& transform) {
            sceneTransform = transform;
        }

        /**
         * Set the current condition info for display and recording purposes.
         * Should be called whenever the condition changes (e.g., when loading a new
         *   model or changing settings).
         */
        void setConditionInfo(int conditionIndex, int totalConditions, RasterizerVariant modelType,
                              const std::string& modelPath, bool antiAliasing) {
            this->currentConditionIndex = conditionIndex;
            this->conditionInfo = "Condition " + std::to_string(conditionIndex) + " of " + std::to_string(totalConditions) +
                                  " (" + (modelType == RasterizerVariant::HTGS ? "HTGS" : "HTGS_FOVEATED") + ", "
                                  + modelPath + ", AA: " + (antiAliasing ? "ON" : "OFF") + ")";

            // Record scene change if recording is active
            if (recordingWriter) {
                recordingWriter->writeSceneChange(
                    conditionIndex,
                    static_cast<htgs::recording::RasterizerVariant>(modelType),
                    modelPath,
                    currentSceneScale,
                    this->antiAliasing,
                    this->blurPeriphery
                );
            }
        }

        /**
         * Get the current calibrated model scale factor
         */
        float getCalibratedModelScale() const {
            return calibrationData.modelScale;
        }

        /**
         * Get the current calibrated model transform matrix
         */
        sibr::Matrix4f getSceneTransform() const {
            return sceneTransform;
        }

        /**
         * Process controller input for calibration adjustments (scene scale, yaw and position,
         * written to sceneTransform). Y/B cycles through the modes, see CalibrationControl.
         */
        void handleCalibrationInput(const sibr::ControllerState& controller);

        /**
         * Process controller input for free locomotion relative to the head direction.
         * Left stick: move / strafe horizontally. Right stick: x turns (around the head),
         * y moves up / down. Y/B resets. Does not modify sceneTransform.
         */
        void handleLocomotionInput(const sibr::ControllerState& controller);

        /** Undo all locomotion (back to the pose given by sceneTransform). */
        void resetLocomotion() { locomotionYaw = 0.0f; locomotionTranslation.setZero(); }

        /** What the stick currently adjusts in calibration mode. */
        enum class CalibrationControl : int { SCALE = 0, YAW = 1, POSITION_HORIZONTAL = 2, HEIGHT = 3, COUNT = 4 };
        CalibrationControl getCalibrationControl() const { return calibrationControl; }
        static const char* calibrationControlDescription(CalibrationControl control);

        /**
         * Start a new recording session, which stores parameters for each rendered frame,
         * plus any scene switches and changes to the masks
         */
        void enableRecording(const std::string& filename);

        /**
         * Stop the current recording session and save the recording file.
         */
        void disableRecording();

        /**
         * Announce the OpenXR timing of the headset frame about to be rendered
         * (see OpenXRRdrMode::setFrameBeginCallback). If recording, it is written as a
         * FRAME_BEGIN record before the frame's first FRAME_RENDER record.
         */
        void beginXRFrame(int64_t displayTime, int64_t displayPeriod) {
            pendingFrameBegin = htgs::recording::FrameBeginRecord{displayTime, displayPeriod};
        }

        // Rasterizer settings
        RasterizerVariant modelType = RasterizerVariant::HTGS_FOVEATED;
        bool antiAliasing = true;
        bool blurPeriphery = true;
        bool exportVisibilityMask = false; // If true, will export the visibility mask for each eye as a line loop
        bool stereoRendering = true;  // Render both eyes in one stereo call when the stereo hook is called and stereo applies

    protected:
        // OpenGL / CUDA resources
        int _device = 0;
        BufferCopyRenderer* _copyRenderer;

        // Per-eye GPU state, indexed by eye slot (eyeSlot(), never a raw view index).
        // Slot 0 is used by mono rendering; slot 1 is allocated on first stereo use.
        EyeGpuState eyeGpu[2];

        // Stereo frame state: set by onPrepareStereoFrame, consumed by onRenderIBR
        struct StereoReadyImage {
            bool ready = false;
            EyeRenderParams params;  // Params the image was rendered with (must match the per-eye call)
            RasterizerVariant modelType = RasterizerVariant::HTGS_FOVEATED;
            bool antiAliasing = false;
            bool blurPeriphery = false;
        };
        StereoReadyImage stereoReady[2];

        /** Whether the stereo hook renders this frame in stereo. ExperimentView adds its own conditions. */
        virtual bool stereoApplies() const;

        // Model data
        int count;
        float* posCuda = nullptr;
        float* rotCuda = nullptr;
        float* scaleCuda = nullptr;
        float* opacityCuda = nullptr;
        float* shs0Cuda = nullptr;
        float* shsRestCuda = nullptr;
        float* bgModelDataCuda = nullptr;
        BackgroundModelType backgroundModelType = BackgroundModelType::NONE;
        int2 backgroundTextureResolution = {0, 0};  // Only used for texture background models
        sibr::Matrix4f sceneTransform = sibr::Matrix4f::Identity();

        // Frame-specific pose data
        bool gazeVisualization = false;

        // Mask data for foveation / skipping tiles not visible in VR
        bool exportedVisibilityMask[2] = {false, false}; // Track whether we've exported the mask for each eye to avoid redundant exports
        uint32_t visibilityMaskSize[2][2] = {{0, 0}, {0, 0}};
        uint32_t* visibilityMask[2] = {nullptr, nullptr};
        uint32_t* visibilityMaskAreaTable[2] = {nullptr, nullptr};
        uint32_t* foveaMaskAreaTable = nullptr;

        // Calibration Data
        std::string conditionInfo;
        bool calibrationMode = false;
        struct {
            float baseModelScale = 1.0f;
            float modelScale = 1.0f;
            std::vector<Pos> positions;  // Need to be stored for rescaling
            std::vector<Scale> scales;
        } calibrationData;
        float heightAdjustment = 0.0f;
        CalibrationControl calibrationControl = CalibrationControl::SCALE;

        // Locomotion: a yaw around the world's vertical axis plus a translation, applied in world
        // space on top of sceneTransform (model -> world = L * sceneTransform). Kept as yaw +
        // translation instead of a matrix so repeated updates can't drift away from a rigid motion.
        float locomotionYaw = 0.0f;
        sibr::Vector3f locomotionTranslation = sibr::Vector3f::Zero();
        float locomotionSpeed = 1.5f;      // m/s at full stick deflection
        float locomotionTurnSpeed = 90.0f; // deg/s at full stick deflection
        std::chrono::steady_clock::time_point lastInputTime{};
        sibr::Matrix4f locomotionTransform() const;

        // Head pose in world space (the frame sceneTransform maps into; with OpenXRRdrMode's
        // default Y-invert this is +x right, +y down, +z initially forward), taken from the last
        // rendered eye cameras, so it includes every offset the renderer actually uses.
        sibr::Vector3f eyeWorldPosition[2] = { sibr::Vector3f::Zero(), sibr::Vector3f::Zero() };
        sibr::Vector3f eyeWorldForward[2] = { sibr::Vector3f(0, 0, 1), sibr::Vector3f(0, 0, 1) };
        sibr::Vector3f eyeWorldUp[2] = { sibr::Vector3f(0, -1, 0), sibr::Vector3f(0, -1, 0) };
        bool stereoEyesSeen = false;  // Only slot 0 is filled in mono (desktop) rendering
        sibr::Vector3f headWorldPosition() const {
            return stereoEyesSeen ? sibr::Vector3f(0.5f * (eyeWorldPosition[0] + eyeWorldPosition[1])) : eyeWorldPosition[0];
        }
        /** Head-relative horizontal frame: unit forward / right vectors (in the floor plane) and world up. */
        struct HeadFrame { sibr::Vector3f forward, right, up, position; };
        HeadFrame headFrame() const;
        float inputDeltaTime();
        void uploadScaledModel();

        // Recording data
        std::unique_ptr<htgs::recording::RecordingWriter> recordingWriter;
        std::optional<htgs::recording::FrameBeginRecord> pendingFrameBegin;  // written lazily, see beginXRFrame()
        std::string currentModelPath;
        float currentSceneScale = 1.0f;
        int currentConditionIndex = -1;

        void allocateEyeGpuState(EyeGpuState& state);
        void freeEyeGpuState(EyeGpuState& state);
        void createImageBuffer(EyeGpuState& state);
        void destroyImageBuffer(EyeGpuState& state);
        void invalidateStereoImages() { stereoReady[0].ready = stereoReady[1].ready = false; }

        /**
         * Render one eye with the mono rasterizer (HTGS or VR-HTGS) into state's image buffer.
         */
        void rasterizeMono(const EyeRenderParams& params, EyeGpuState& state);
        /** Render both eyes with one VR-HTGS stereo call into eyeGpu[0] / eyeGpu[1]. */
        void rasterizeStereo(const EyeRenderParams& left, const EyeRenderParams& right);

        /** Per-eye parameters for the current resolution, scene transform and locomotion. */
        EyeRenderParams computeEyeRenderParams(const sibr::Camera& eye) const;

        void updateVisibilityMask(const sibr::Camera& eye, const uint32_t width, const uint32_t height);
        void initializeFoveaMask();
    };
} /*namespace sibr*/
