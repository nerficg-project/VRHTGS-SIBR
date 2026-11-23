#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <sstream>
#include <thread>

#include "boost/asio.hpp"
#include "imgui_internal.h"
#include <openxr/openxr.h>
#include <tinyexr.h>

#include "core/graphics/GUI.hpp"
#include "core/openxr/ControllerState.hpp"
#include "VRHTGSCudaBackend/rasterization/include/inference.h"
#include "VRHTGSCudaBackend/rasterization/include/inference_stereo.h"
#include "VRHTGSCudaBackend/rasterization/include/utils/rasterization_utils.h"
#include "VRHTGSCudaBackend/visualization/include/visualization.h"
#include "HTGSCudaBackend/rasterization/include/hybrid_blend/fast_inference.h"
#include "projects/vrhtgs_viewer/renderer/GaussianView.hpp"


static float sigmoid(const float m1) {
    return 1.0f / (1.0f + exp(-m1));
}

static float inverse_sigmoid(const float m1) {
    return log(m1 / (1.0f - m1));
}


#define CUDA_SAFE_CALL_ALWAYS(A) \
        A; \
        cudaDeviceSynchronize(); \
        if (cudaPeekAtLastError() != cudaSuccess) \
        SIBR_ERR << cudaGetErrorString(cudaGetLastError());

#if DEBUG || _DEBUG
    #define CUDA_SAFE_CALL(A) CUDA_SAFE_CALL_ALWAYS(A)
#else
    #define CUDA_SAFE_CALL(A) A
#endif


namespace sibr {
    // A simple copy renderer class
    class BufferCopyRenderer {
    public:
        BufferCopyRenderer() {
            _shader.init("CopyShader",
                    sibr::loadFile(sibr::getShadersDirectory("htgs-renderer") + "/copy.vert"),
                    sibr::loadFile(sibr::getShadersDirectory("htgs-renderer") + "/copy.frag"));

            _flip.init(_shader, "flip");
            _width.init(_shader, "width");
            _height.init(_shader, "height");
        }

        void process(uint32_t bufferID, IRenderTarget& dst, int width,
                int height, bool disableTest = true)
        {
            if (disableTest) {
                glDisable(GL_DEPTH_TEST);
            } else {
                glEnable(GL_DEPTH_TEST);
            }

            _shader.begin();
            _flip.send();
            _width.send();
            _height.send();

            // No clear needed: the full-screen copy overwrites every pixel (alpha 1, blending off)
            dst.bind();
            glDisable(GL_BLEND);

            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufferID);

            sibr::RenderUtility::renderScreenQuad();

            dst.unbind();
            _shader.end();
        }

        /** \return option to flip the texture when copying. */
        bool& flip() { return _flip.get(); }
        int& width() { return _width.get(); }
        int& height() { return _height.get(); }

    private:
        GLShader _shader;
        GLuniform<bool> _flip = false; ///< Flip the texture when copying.
        GLuniform<int> _width = 1000;
        GLuniform<int> _height = 800;
    };
}


// ======= Helper Functions ========

// Load the Gaussians from the given file.
template<int D>
static int loadPly(
    const char* filename,
    std::vector<sibr::Pos>& pos,
    std::vector<float>& shs0,
    std::vector<float>& shsRest,
    std::vector<float>& opacities,
    std::vector<sibr::Scale>& scales,
    std::vector<sibr::Rot>& rot,
    const float modelScale,
    BackgroundModelType& backgroundModelType,
    float*& bgModelDataCuda,
    int2& backgroundTextureResolution
) {
    std::ifstream infile(filename, std::ios_base::binary);
    if (!infile.good()) {
        SIBR_ERR << "Unable to find model's PLY file, attempted:\n" << filename << std::endl;
    }

    // Parse header: accept comments/obj_info lines and look up vertex properties by name,
    // so files with or without normals (nx, ny, nz) or extra properties load correctly.
    std::string buff;
    std::getline(infile, buff);
    if (buff.rfind("ply", 0) != 0) {
        SIBR_ERR << "Not a PLY file: " << filename << std::endl;
    }

    int count = 0;
    bool inVertexElement = false;
    std::vector<std::string> propertyNames;
    while (std::getline(infile, buff)) {
        if (!buff.empty() && buff.back() == '\r') {
            buff.pop_back();
        }
        std::stringstream ss(buff);
        std::string keyword;
        ss >> keyword;
        if (keyword == "end_header") {
            break;
        } else if (keyword == "format") {
            std::string format;
            ss >> format;
            if (format != "binary_little_endian") {
                SIBR_ERR << "Unsupported PLY format '" << format << "' in " << filename << ", expected binary_little_endian" << std::endl;
            }
        } else if (keyword == "element") {
            std::string elementName;
            ss >> elementName;
            inVertexElement = (elementName == "vertex");
            if (inVertexElement) {
                ss >> count;
            }
        } else if (keyword == "property" && inVertexElement) {
            std::string type, name;
            ss >> type >> name;
            if (type != "float" && type != "float32") {
                SIBR_ERR << "Unsupported PLY property type '" << type << "' for '" << name << "' in " << filename << ", expected float" << std::endl;
            }
            propertyNames.push_back(name);
        }
        // Other lines (comment, obj_info, ...) are ignored
    }

    // Output number of Gaussians contained
    SIBR_LOG << "Loading " << count << " Gaussian splats" << std::endl;

    const size_t stride = propertyNames.size();
    auto propertyIndex = [&](const std::string& name, bool required = true) -> int {
        auto it = std::find(propertyNames.begin(), propertyNames.end(), name);
        if (it == propertyNames.end()) {
            if (required) {
                SIBR_ERR << "PLY file " << filename << " is missing required property '" << name << "'" << std::endl;
            }
            return -1;
        }
        return static_cast<int>(it - propertyNames.begin());
    };

    const int SH_N = (D + 1) * (D + 1);
    const int posIdx[3] = { propertyIndex("x"), propertyIndex("y"), propertyIndex("z") };
    const int dcIdx[3] = { propertyIndex("f_dc_0"), propertyIndex("f_dc_1"), propertyIndex("f_dc_2") };
    const int opacityIdx = propertyIndex("opacity");
    const int scaleIdx[3] = { propertyIndex("scale_0"), propertyIndex("scale_1"), propertyIndex("scale_2") };
    const int rotIdx[4] = { propertyIndex("rot_0"), propertyIndex("rot_1"), propertyIndex("rot_2"), propertyIndex("rot_3") };
    std::vector<int> restIdx(3 * (SH_N - 1));
    for (int j = 0; j < 3 * (SH_N - 1); j++) {
        restIdx[j] = propertyIndex("f_rest_" + std::to_string(j), false);  // Missing higher-order SHs default to 0
    }

    // Read all Gaussians at once (AoS, all float properties)
    std::vector<float> raw(static_cast<size_t>(count) * stride);
    infile.read(reinterpret_cast<char*>(raw.data()), raw.size() * sizeof(float));
    if (static_cast<size_t>(infile.gcount()) != raw.size() * sizeof(float)) {
        SIBR_ERR << "PLY file " << filename << " is truncated: expected " << count << " vertices with "
                 << stride << " float properties each" << std::endl;
    }

    // Resize our SoA data
    pos.resize(count);
    shs0.resize(3 * count);
    shsRest.resize(45 * count);
    scales.resize(count);
    rot.resize(count);
    opacities.resize(count);
    std::fill(shsRest.begin(), shsRest.end(), 0.0f);

    // Move data from AoS to SoA
    const float logModelScale = log(modelScale);
    for (int i = 0; i < count; i++) {
        const float* p = raw.data() + static_cast<size_t>(i) * stride;

        pos[i] = sibr::Pos(p[posIdx[0]], p[posIdx[1]], p[posIdx[2]]) * modelScale;

        // Normalize quaternion
        float length2 = 0;
        for (int j = 0; j < 4; j++) {
            length2 += p[rotIdx[j]] * p[rotIdx[j]];
        }
        float length = sqrt(length2);
        for (int j = 0; j < 4; j++) {
            rot[i].rot[j] = p[rotIdx[j]] / length;
        }

        // Exponentiate scale
        for (int j = 0; j < 3; j++) {
            scales[i].scale[j] = exp(p[scaleIdx[j]] + logModelScale);
        }

        // Activate alpha
        opacities[i] = sigmoid(p[opacityIdx]);

        shs0[i * 3 + 0] = p[dcIdx[0]];
        shs0[i * 3 + 1] = p[dcIdx[1]];
        shs0[i * 3 + 2] = p[dcIdx[2]];

        // f_rest is stored channel-major (all R coeffs, then G, then B); we store coefficient-major (interleaved RGB)
        for (int j = 1; j < SH_N; j++) {
            for (int c = 0; c < 3; c++) {
                const int idx = restIdx[(j - 1) + c * (SH_N - 1)];
                if (idx >= 0) {
                    shsRest[i * 45 + (j - 1) * 3 + c] = p[idx];
                }
            }
        }
    }

    // Check for background model data
    std::filesystem::path pth(filename);
    std::filesystem::path bgShPth = pth.parent_path() / "background_sh_coefficients.bin";
    std::filesystem::path bgTexturePth = pth.parent_path() / "background_texture.png";
    std::filesystem::path bgTextureRawPth = pth.parent_path() / "background_texture.exr";

    if (std::filesystem::exists(bgShPth)) {
        SIBR_LOG << "Background SH coefficients found at " << bgShPth.string() << "\n";
        // Open file
        std::ifstream shFile(bgShPth, std::ios::binary);
        if (!shFile.good()) {
            SIBR_ERR << "Unable to open background SH coefficients file at " << bgShPth.string() << "\n";
        }
        // Read data
        size_t shSize = sizeof(float) * 48; // 3 channels, 16 coeffs each (up to 3rd order)
        std::array<float, 48> backgroundShCoeffs;
        shFile.read(reinterpret_cast<char*>(backgroundShCoeffs.data()), shSize);
        // Allocate GPU memory and copy data
        if (bgModelDataCuda) {
            CUDA_SAFE_CALL(cudaFree(bgModelDataCuda));
        }
        CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&bgModelDataCuda), shSize));
        CUDA_SAFE_CALL(cudaMemcpy(bgModelDataCuda, backgroundShCoeffs.data(), shSize, cudaMemcpyHostToDevice));
        backgroundModelType = BackgroundModelType::SH;
    } else if (std::filesystem::exists(bgTexturePth)) {
        SIBR_LOG << "Background texture found at " << bgTexturePth.string() << "\n";
        // Load image using OpenCV
        sibr::ImageRGB32F bgImage;
        if (!bgImage.load(bgTexturePth.string(), false)) {
            SIBR_ERR << "Unable to load background texture at " << bgTexturePth.string() << "\n";
        }
        // Allocate GPU memory and copy data.
        // The host image is HWC (interleaved RGB floats). We want CHW (planar) on the device.
        size_t W = static_cast<size_t>(bgImage.w());
        size_t H = static_cast<size_t>(bgImage.h());
        size_t numPixels = W * H;
        size_t planeSize = sizeof(float) * numPixels; // bytes per channel plane

        if (bgModelDataCuda) {
            CUDA_SAFE_CALL(cudaFree(bgModelDataCuda));
        }
        CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&bgModelDataCuda), planeSize * 3));

        // Copy each channel separately using cudaMemcpy2D. We treat the host as
        // an array of 'numPixels' rows where each row contains a single float
        // for the channel and the source pitch is 3 * sizeof(float) (RGB interleaved).
        for (int c = 0; c < 3; ++c) {
            const float* srcPtr = reinterpret_cast<const float*>(bgImage.data()) + c;
            float* dstPtr = bgModelDataCuda + numPixels * c;

            CUDA_SAFE_CALL(cudaMemcpy2D(
                dstPtr,                     // dst
                sizeof(float),              // dpitch (bytes between rows in dst)
                srcPtr,                     // src
                3 * sizeof(float),          // spitch (bytes between rows in src)
                sizeof(float),              // width (bytes per row)
                numPixels,
                cudaMemcpyHostToDevice
            ));
        }

        backgroundModelType = BackgroundModelType::TEXTURE;
        backgroundTextureResolution = make_int2(static_cast<int>(W), static_cast<int>(H));
    } else if (std::filesystem::exists(bgTextureRawPth)) {
        SIBR_LOG << "Raw background texture found at " << bgTextureRawPth.string() << "\n";
        // Lat-long texture of pre-activation values (the sigmoid is applied by the rasterizer after
        // interpolation). tinyexr converts the half-float channels to float and returns RGBA
        // interleaved (A = 1 for an RGB file), rows top to bottom like the PNG path.
        float* rgba = nullptr;
        int width = 0;
        int height = 0;
        const char* err = nullptr;
        if (LoadEXR(&rgba, &width, &height, bgTextureRawPth.string().c_str(), &err) != TINYEXR_SUCCESS) {
            const std::string message = err ? err : "unknown error";
            FreeEXRErrorMessage(err);
            SIBR_ERR << "Unable to load raw background texture at " << bgTextureRawPth.string() << ": " << message << "\n";
        }

        // Upload CHW (planar), like the PNG path.
        const size_t numPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (bgModelDataCuda) {
            CUDA_SAFE_CALL(cudaFree(bgModelDataCuda));
        }
        CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&bgModelDataCuda), sizeof(float) * numPixels * 3));
        for (int c = 0; c < 3; ++c) {
            CUDA_SAFE_CALL(cudaMemcpy2D(
                bgModelDataCuda + numPixels * c,  // dst
                sizeof(float),                    // dpitch
                rgba + c,                         // src
                4 * sizeof(float),                // spitch (RGBA interleaved)
                sizeof(float),                    // width (bytes per row)
                numPixels,
                cudaMemcpyHostToDevice
            ));
        }
        free(rgba);

        backgroundModelType = BackgroundModelType::TEXTURE_RAW;
        backgroundTextureResolution = make_int2(width, height);
    } else {
        SIBR_LOG << "No background model data found\n";
        // Free a previous model's background data
        if (bgModelDataCuda) {
            CUDA_SAFE_CALL(cudaFree(bgModelDataCuda));
            bgModelDataCuda = nullptr;
        }
        backgroundModelType = BackgroundModelType::NONE;
    }

    return count;
}

static std::function<char*(size_t)> getBufferGrowFunction(sibr::RasterizationBuffer* buffer, float overallocationFactor = 1.0f) {
    return [buffer, overallocationFactor](size_t N) {
        if (buffer->size >= N) {
            return buffer->data;
        }

        char* newData = nullptr;
        if (overallocationFactor > 1.0f) {
            N = static_cast<size_t>(N * overallocationFactor);
        }
        cudaMalloc(&newData, N);

        if (buffer->data) {
            cudaFree(buffer->data);
        }
        buffer->data = newData;
        buffer->size = N;
        return buffer->data;
    };
}

static std::function<char*(size_t)> getBufferResizeFunction(sibr::RasterizationBuffer* buffer, float overallocationFactor = 1.0f, float shrinkThreshold = 0.75f) {
    return [buffer, overallocationFactor, shrinkThreshold](size_t N) {
        if (buffer->size >= N && buffer->size <= N * (1.0f / shrinkThreshold)) {
            return buffer->data;
        }

        char* newData = nullptr;
        if (overallocationFactor > 1.0f) {
            N = static_cast<size_t>(N * overallocationFactor);
        }
        cudaMalloc(&newData, N);

        if (buffer->data) {
            cudaFree(buffer->data);
        }
        buffer->data = newData;
        buffer->size = N;
        return buffer->data;
    };
}


static std::vector<uint32_t> calculateSummedAreaTable(
    const std::vector<uint8_t>& mask,
    const uint32_t width,
    const uint32_t height
) {
    std::vector<uint32_t> areaTable((width + 1) * (height + 1), 0);
    for (uint32_t y = 0; y < height + 1; y++) {
        for (uint32_t x = 0; x < width + 1; x++) {
            uint32_t idx = y * (width + 1) + x;
            uint32_t area = 0;

            if (x > 0) {
                area += areaTable[y * (width + 1) + (x - 1)];
            }
            if (y > 0) {
                area += areaTable[(y - 1) * (width + 1) + x];
            }
            if (x > 0 && y > 0) {
                area -= areaTable[(y - 1) * (width + 1) + (x - 1)];
                area += mask[(y - 1) * width + (x - 1)];
            }

            areaTable[idx] = area;
        }
    }

    return areaTable;
}

static bool pointInVisibilityMask(std::vector<XrVector2f>& visibilityMaskPoints, float u, float v) {
    if (visibilityMaskPoints.empty()) {
        return true; // No mask defined, consider all points visible
    }

    // Assumption: Mask is a convex line loop
    XrVector2f point = { u, v };
    bool sign = false;
    for (int i = 0; i < visibilityMaskPoints.size(); i++) {
        XrVector2f a = visibilityMaskPoints[i];
        XrVector2f b = visibilityMaskPoints[(i + 1) % visibilityMaskPoints.size()];
        float cross = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
        if (i == 0) {
            sign = cross > 0;
        } else if ((cross > 0) != sign) {
            return false;
        }
    }

    return true;
}


sibr::HTGaussianView::HTGaussianView(
        uint32_t renderW, uint32_t renderH, const char* file,
        const float scale, int device, bool calibrationMode) :
    sibr::ViewBase(renderW, renderH),
    calibrationMode(calibrationMode)
{
    // Set up CUDA device and context
    int numDevices;
    CUDA_SAFE_CALL_ALWAYS(cudaGetDeviceCount(&numDevices));

    _device = device;
    if (device >= numDevices) {
        if (numDevices == 0) {
            SIBR_ERR << "No CUDA devices detected!";
        } else {
            SIBR_ERR << "Provided device index exceeds number of available CUDA devices!";
        }
    }
    CUDA_SAFE_CALL_ALWAYS(cudaSetDevice(device));

    cudaDeviceProp prop;
    CUDA_SAFE_CALL_ALWAYS(cudaGetDeviceProperties(&prop, device));
    if (prop.major < 8 || (prop.major == 8 && prop.minor < 9)) {
        SIBR_WRG << "Code has not been tested with compute capability < 8.9! Results may vary.";
    }

    // Initialize copy renderer
    _copyRenderer = new BufferCopyRenderer();
    _copyRenderer->flip() = true;
    _copyRenderer->width() = _resolution.x();
    _copyRenderer->height() = _resolution.y();

    // Load model and set up rendering buffers (eye slot 1 is allocated on first stereo use)
    loadModel(file, scale);
    allocateEyeGpuState(eyeGpu[0]);
    initializeFoveaMask();

    // Pinned host staging for the view parameter uploads (one slot per eye slot, see uploadPose)
    CUDA_SAFE_CALL_ALWAYS(cudaMallocHost(reinterpret_cast<void**>(&viewParamsHost), 2 * EyeGpuState::viewParamsFloats * sizeof(float)));
}


void sibr::HTGaussianView::allocateEyeGpuState(EyeGpuState& state) {
    if (state.allocated) {
        return;
    }

    // Set up resize functions for the rasterizer buffers
    state.perPrimitiveBuffer.resizeFunc = getBufferGrowFunction(&state.perPrimitiveBuffer);
    state.perTileBuffer.resizeFunc = getBufferGrowFunction(&state.perTileBuffer);
    state.perSubtileBuffer.resizeFunc = getBufferGrowFunction(&state.perSubtileBuffer);
    // Since this is the only buffer that changes while rendering the same scene,
    //   we overallocate by 1.25x to reduce the number of necessary resizes.
    state.perInstanceBuffer.resizeFunc = getBufferGrowFunction(&state.perInstanceBuffer, 1.25);
    // Special buffer to contain the intermediate image for blurring
    state.blurImageBuffer.resizeFunc = getBufferResizeFunction(&state.blurImageBuffer, 1.1, 0.75);

    // GPU memory for view parameters
    // One device block for view | proj | invRVP | camPos (offsets are 64-byte aligned for float4 access)
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&state.viewCuda), EyeGpuState::viewParamsFloats * sizeof(float)));
    state.projCuda = state.viewCuda + 16;
    state.invRVPCuda = state.viewCuda + 32;
    state.camPosCuda = state.viewCuda + 48;

    createImageBuffer(state);
    state.allocated = true;
}

void sibr::HTGaussianView::freeEyeGpuState(EyeGpuState& state) {
    if (!state.allocated) {
        return;
    }

    destroyImageBuffer(state);
    cudaFree(state.viewCuda);  // projCuda / invRVPCuda / camPosCuda point into the same block
    state.viewCuda = state.projCuda = state.invRVPCuda = state.camPosCuda = nullptr;
    state.perPrimitiveBuffer.release();
    state.perTileBuffer.release();
    state.perSubtileBuffer.release();
    state.perInstanceBuffer.release();
    state.blurImageBuffer.release();
    state.allocated = false;
}


void sibr::HTGaussianView::setResolution(const Vector2i &size) {
    if (size != getResolution()) {
        SIBR_LOG << "Set resolution => " << size << std::endl;
        ViewBase::setResolution(size);
        invalidateStereoImages();
        for (EyeGpuState& state : eyeGpu) {
            if (state.allocated) {
                destroyImageBuffer(state);
                createImageBuffer(state);
            }
        }

        _copyRenderer->width() = _resolution.x();
        _copyRenderer->height() = _resolution.y();
    }
}

void sibr::HTGaussianView::loadModel(const char* file, float modelScale) {
    invalidateStereoImages();

    // Track current model for recording
    currentModelPath = file;
    currentSceneScale = modelScale;

    // Load the PLY data (AoS) to the GPU (SoA)
    std::vector<Pos> pos;
    std::vector<Rot> rot;
    std::vector<Scale> scale;
    std::vector<float> opacity;
    std::vector<float> shs0;
    std::vector<float> shsRest;
    count = loadPly<3>(file, pos, shs0, shsRest, opacity, scale, rot, modelScale, backgroundModelType, bgModelDataCuda, backgroundTextureResolution);

    // Free existing GPU data if any
    CUDA_SAFE_CALL_ALWAYS(cudaFree(posCuda));
    CUDA_SAFE_CALL_ALWAYS(cudaFree(rotCuda));
    CUDA_SAFE_CALL_ALWAYS(cudaFree(shs0Cuda));
    CUDA_SAFE_CALL_ALWAYS(cudaFree(shsRestCuda));
    CUDA_SAFE_CALL_ALWAYS(cudaFree(opacityCuda));
    CUDA_SAFE_CALL_ALWAYS(cudaFree(scaleCuda));

    // Allocate and fill the GPU data
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&posCuda), sizeof(Pos) * count));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(posCuda, pos.data(), sizeof(Pos) * count, cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&rotCuda), sizeof(Rot) * count));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(rotCuda, rot.data(), sizeof(Rot) * count, cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&shs0Cuda), sizeof(float) * 3 * count));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(shs0Cuda, shs0.data(), sizeof(float) * 3 * count, cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&shsRestCuda), sizeof(float) * 45 * count));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(shsRestCuda, shsRest.data(), sizeof(float) * 45 * count, cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&opacityCuda), sizeof(float) * count));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(opacityCuda, opacity.data(), sizeof(float) * count, cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&scaleCuda), sizeof(Scale) * count));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(scaleCuda, scale.data(), sizeof(Scale) * count, cudaMemcpyHostToDevice));

    // Keep CPU copies for calibration mode
    if (calibrationMode) {
        calibrationData.baseModelScale = calibrationData.modelScale = modelScale;
        calibrationData.positions = std::move(pos);
        calibrationData.scales = std::move(scale);
    }
}


void sibr::HTGaussianView::createImageBuffer(EyeGpuState& state) {
    // Create GL buffer ready for CUDA/GL interop
    glCreateBuffers(1, &state.imageBuffer);
    glNamedBufferStorage(state.imageBuffer, _resolution.x() * _resolution.y() * 3 * sizeof(float), nullptr, GL_DYNAMIC_STORAGE_BIT);

    if (cudaPeekAtLastError() != cudaSuccess) {
        SIBR_ERR << "A CUDA error occurred in setup:" << cudaGetErrorString(cudaGetLastError()) << ". Please rerun in Debug to find the exact line!";
    }
    cudaGraphicsGLRegisterBuffer(&state.imageBufferCuda, state.imageBuffer, cudaGraphicsRegisterFlagsWriteDiscard);
    if (cudaPeekAtLastError() != cudaSuccess) {
        SIBR_ERR << "A CUDA error occurred in setup:" << cudaGetErrorString(cudaGetLastError()) << ". Please rerun in Debug to find the exact line!";
    }
}

void sibr::HTGaussianView::destroyImageBuffer(EyeGpuState& state) {
    if (state.imageBufferCuda) {
        cudaGraphicsUnregisterResource(state.imageBufferCuda);
        state.imageBufferCuda = nullptr;
    }
    glDeleteBuffers(1, &state.imageBuffer);
    state.imageBuffer = 0;
}


// Per-eye slot for the visibility mask arrays (index 0 for mono rendering modes / unexpected view indices)
static uint32_t eyeSlot(const sibr::Camera& eye) {
    return eye.viewIndex() < 2 ? eye.viewIndex() : 0;
}

void sibr::HTGaussianView::updateVisibilityMask(const sibr::Camera& eye, const uint32_t width, const uint32_t height) {
    const uint32_t side = eyeSlot(eye);
    const uint32_t widthTiles = (width + vrhtgs::rasterization::config::tile_width_large - 1) / vrhtgs::rasterization::config::tile_width_large;
    const uint32_t heightTiles = (height + vrhtgs::rasterization::config::tile_height_large - 1) / vrhtgs::rasterization::config::tile_height_large;
    if (widthTiles == visibilityMaskSize[side][0] && heightTiles == visibilityMaskSize[side][1]) {
        return;
    }

    // Free old GPU memory (doesn't do anything if null)
    CUDA_SAFE_CALL(cudaFree(visibilityMask[side]));
    CUDA_SAFE_CALL(cudaFree(visibilityMaskAreaTable[side]));
    visibilityMask[side] = nullptr;
    visibilityMaskSize[side][0] = widthTiles;
    visibilityMaskSize[side][1] = heightTiles;
    visibilityMaskAreaTable[side] = nullptr;

    // Determine visibility mask points in [0,1]x[0,1]
    std::vector<XrVector2f> visibilityMaskPoints;
    for (uint32_t i = 0; i < eye.visibilityMask().vertexCountOutput; i++) {
        const XrVector2f& vertex = eye.visibilityMask().vertices[i];
        visibilityMaskPoints.emplace_back(
            (vertex.x - eye.boundsOnViewPlane().x()) / (eye.boundsOnViewPlane().y() - eye.boundsOnViewPlane().x()),
            (vertex.y - eye.boundsOnViewPlane().z()) / (eye.boundsOnViewPlane().w() - eye.boundsOnViewPlane().z())
        );
    }

    // Export visibility mask if enabled
    if (exportVisibilityMask && !exportedVisibilityMask[side]) {
        const std::string filename = "render_mask_view_" + std::to_string(side) + ".bin";
        std::filesystem::path filepath(filename);

        std::ofstream outfile(filepath, std::ios_base::binary);
        for (const auto& pt : visibilityMaskPoints) {
            outfile.write(reinterpret_cast<const char*>(&pt), sizeof(XrVector2f));
        }
        outfile.close();
        exportedVisibilityMask[side] = true;
    }

    // Regenerate visibility mask
    std::vector<uint8_t> visibilityMaskRaw(widthTiles * heightTiles, 1);
    for (uint32_t y = 0; y < heightTiles; ++y) {
        for (uint32_t x = 0; x < widthTiles; ++x) {
            // Compute tile center in pixel coordinates
            float tileCenterX = (x + 0.5f) * vrhtgs::rasterization::config::tile_width_large;
            float tileCenterY = (y + 0.5f) * vrhtgs::rasterization::config::tile_height_large;

            // Normalize to [0,1]
            float normX = tileCenterX / static_cast<float>(width);
            float normY = tileCenterY / static_cast<float>(height);

            // Check against visibility mask
            if (!pointInVisibilityMask(visibilityMaskPoints, normX, normY)) {
                visibilityMaskRaw[y * widthTiles + x] = 0;
            }
        }
    }

    // Calculate summed area table for visibility mask
    std::vector<uint32_t> visibilityMaskAreaTableHost =
        calculateSummedAreaTable(visibilityMaskRaw, widthTiles, heightTiles);

    // Pack mask into uint32_t array, 32 tiles per word (tile i -> word i / 32, bit i % 32)
    constexpr size_t bitsPerWord = sizeof(uint32_t) * 8;
    std::vector<uint32_t> visibilityMaskPacked(
        (widthTiles * heightTiles + bitsPerWord - 1) / bitsPerWord,
        0
    );
    for (size_t i = 0; i < visibilityMaskRaw.size(); ++i) {
        visibilityMaskPacked[i / 32] |= (visibilityMaskRaw[i] << (i % 32));
    }

    // Upload to GPU
    CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&visibilityMask[side]), sizeof(uint32_t) * visibilityMaskPacked.size()));
    CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&visibilityMaskAreaTable[side]), sizeof(uint32_t) * visibilityMaskAreaTableHost.size()));
    CUDA_SAFE_CALL(cudaMemcpy(visibilityMask[side], visibilityMaskPacked.data(), sizeof(uint32_t) * visibilityMaskPacked.size(), cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL(cudaMemcpy(visibilityMaskAreaTable[side], visibilityMaskAreaTableHost.data(), sizeof(uint32_t) * visibilityMaskAreaTableHost.size(), cudaMemcpyHostToDevice));

    // Record the mask change if recording is enabled. The record is written when this eye is rendered next
    // (writeVisibilityMaskRecord), not here: the stereo hook updates both eyes' masks before either eye is
    // rendered, and the recording must have the same record order in mono and stereo.
    if (recordingWriter) {
        PendingMaskRecord& record = pendingMaskRecord[side];
        record.pending = true;
        record.widthTiles = widthTiles;
        record.heightTiles = heightTiles;
        record.packedMask = std::move(visibilityMaskPacked);
    }
}

void sibr::HTGaussianView::writeVisibilityMaskRecord(const uint32_t slot) {
    PendingMaskRecord& record = pendingMaskRecord[slot];
    if (record.pending && recordingWriter) {
        recordingWriter->writeVisibilityMaskUpdate(record.widthTiles, record.heightTiles, record.packedMask.data());
    }
    record.pending = false;
}

void sibr::HTGaussianView::initializeFoveaMask() {
    if (foveaMaskAreaTable != nullptr) {
        return;  // Already initialized
    }

    const uint32_t foveaMaskSize = 2 * vrhtgs::rasterization::config::foveation_radius_tiles;
    std::vector<uint8_t> foveaMaskRaw(foveaMaskSize * foveaMaskSize, 0);

    const int32_t radiusSq = vrhtgs::rasterization::config::foveation_radius_tiles * vrhtgs::rasterization::config::foveation_radius_tiles;
    for (uint32_t y = 0; y < foveaMaskSize; ++y) {
        for (uint32_t x = 0; x < foveaMaskSize; ++x) {
            const int32_t dx = x + 0.5 - vrhtgs::rasterization::config::foveation_radius_tiles;
            const int32_t dy = y + 0.5 - vrhtgs::rasterization::config::foveation_radius_tiles;
            const uint32_t distSq = dx * dx + dy * dy;
            if (distSq < radiusSq) {
                foveaMaskRaw[y * foveaMaskSize + x] = 3;
            }
        }
    }

    // Calculate summed area table for fovea mask
    std::vector<uint32_t> foveaMaskAreaTableHost =
        calculateSummedAreaTable(foveaMaskRaw, foveaMaskSize, foveaMaskSize);

    // Upload to GPU
    CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&foveaMaskAreaTable), sizeof(uint32_t) * foveaMaskAreaTableHost.size()));
    CUDA_SAFE_CALL(cudaMemcpy(foveaMaskAreaTable, foveaMaskAreaTableHost.data(), sizeof(uint32_t) * foveaMaskAreaTableHost.size(), cudaMemcpyHostToDevice));

    // Record fovea mask if recording is enabled
    if (recordingWriter) {
        recordingWriter->writeFoveaMaskUpdate(foveaMaskSize, foveaMaskSize, foveaMaskAreaTableHost.data());
    }
}


sibr::EyeRenderParams sibr::HTGaussianView::computeEyeRenderParams(const sibr::Camera& eye) const {
    EyeRenderParams params;
    params.slot = eyeSlot(eye);
    params.eyeIndex = eye.viewIndex();

    // Get focal and center position from eye parameters
    float focalX = 0.5f / std::tan(eye.fovy() / 2.0f) / eye.aspect();
    float focalY = 0.5f / std::tan(eye.fovy() / 2.0f);
    focalX *= _resolution.x();
    focalY *= _resolution.y();

    auto pp = eye.principalPoint();
    float centerX = (1.0f - pp.x()) * _resolution.x() - 0.5f;  // Workaround for flipped eyes
    float centerY = pp.y() * _resolution.y() - 0.5f;

    // Get gaze direction and position in world space
    auto eyeGazeRotation = eye.eyeGazeRotation();
    auto eyeGazePosition = eye.eyeGazePosition();

    // Intersect with z=-1 screen plane
    sibr::Vector3f direction = eyeGazeRotation * sibr::Vector3f(0.0f, 0.0f, -1.0f);
    float t = (1.0f - eyeGazePosition.z()) / direction.z();
    float gaze_x = eyeGazePosition.x() + t * direction.x();
    float gaze_y = eyeGazePosition.y() + t * direction.y();

    // Transform to screen space
    params.gazePos[0] = -gaze_x * focalX + centerX;
    params.gazePos[1] = gaze_y * focalY + centerY;

    // Scene transform including locomotion
    sibr::Matrix4f sceneTransform = locomotionTransform() * this->sceneTransform;
    float near = eye.znear();
    float far = eye.zfar();
    float zRange = far - near;
    float nearFar = far * near;

    // Adjust height because the Quest Pro sometimes misjudges the floor level quite significantly
    sceneTransform(1, 3) += heightAdjustment;

    // Convert view and projection to target coordinate system
    sibr::Matrix4f viewMatRaw = eye.view();
    viewMatRaw.row(1) *= -1;
    viewMatRaw.row(2) *= -1;
    sibr::Matrix4f viewMat = viewMatRaw * sceneTransform;
    sibr::Matrix4f w2cRot = viewMat;
    w2cRot.col(3) = sibr::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);  // Remove translation
    sibr::Matrix4f c2wRot = w2cRot.transpose();

    // Create projection / inverse projection matrices manually to ensure they match HTGS's expected conventions
    auto projMat = sibr::Matrix4f({
        { focalX, 0.0f, centerX, 0.0f },
        { 0.0f, focalY, centerY, 0.0f },
        { 0.0f, 0.0f, (far + near) / zRange, -2.0f * nearFar / zRange },
        { 0.0f, 0.0f, 1.0f, 0.0f }
    });
    auto invProjMat = sibr::Matrix4f({
        { 1.0f / focalX, 0.0f, 0.0f, -1.0f * centerX / focalX },
        { 0.0f, 1.0f / focalY, 0.0f, -1.0f * centerY / focalY },
        { 0.0f, 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, -0.5f * zRange / nearFar, 0.5f * (far + near) / nearFar }
    });
    params.viewProj = (projMat * viewMat).transpose();
    params.invRVP = (c2wRot * invProjMat).transpose();
    params.view = viewMat.transpose();

    // Get camera position & transform
    sibr::Vector4f eyePosRaw = sibr::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
    eyePosRaw.head<3>() = eye.position();
    sibr::Vector4f eyePosTransformed = sceneTransform.inverse() * eyePosRaw;
    params.eyePos = eyePosTransformed.head<3>() / eyePosTransformed.w();

    params.focalX = focalX;
    params.focalY = focalY;
    params.centerX = centerX;
    params.centerY = centerY;
    params.zNear = near;
    params.zFar = far;
    return params;
}


// Backend argument structs for one eye (VR-HTGS mono and stereo calls)
static Buffers makeBuffers(sibr::EyeGpuState& state) {
    return Buffers{
        .per_primitive_buffers_func = state.perPrimitiveBuffer.resizeFunc,
        .per_tile_buffers_func = state.perTileBuffer.resizeFunc,
        .per_subtile_buffers_func = state.perSubtileBuffer.resizeFunc,
        .per_instance_buffers_func = state.perInstanceBuffer.resizeFunc
    };
}

static Pose makePose(const sibr::EyeGpuState& state, const sibr::EyeRenderParams& params) {
    return Pose{
        .M = (float4*)state.viewCuda,
        .VPM = (float4*)state.projCuda,
        .VPR_inv = (float4*)state.invRVPCuda,
        .cam_position = (float3*)state.camPosCuda,
        .gaze_position = (float2*)params.gazePos
    };
}

static Intrinsics makeIntrinsics(const sibr::EyeRenderParams& params, const sibr::Vector2i& resolution) {
    return Intrinsics{
        .width = resolution.x(),
        .height = resolution.y(),
        .focal_x = params.focalX,
        .focal_y = params.focalY,
        .center_x = params.centerX,
        .center_y = params.centerY
    };
}

// Copies one eye's frame-dependent data to the GPU: one async copy from pinned memory instead of four
// synchronous pageable copies. The staging slot is the eye slot (params.slot), so in stereo both eyes'
// uploads are in flight at once without sharing staging memory. The rasterizers synchronize with the
// device before they return, so a staging slot is free again by the time it is reused.
void sibr::HTGaussianView::uploadPose(EyeGpuState& state, const EyeRenderParams& params) {
    float* staging = viewParamsHost + params.slot * EyeGpuState::viewParamsFloats;
    std::memcpy(staging, params.view.data(), 16 * sizeof(float));
    std::memcpy(staging + 16, params.viewProj.data(), 16 * sizeof(float));
    std::memcpy(staging + 32, params.invRVP.data(), 16 * sizeof(float));
    std::memcpy(staging + 48, params.eyePos.data(), 3 * sizeof(float));
    CUDA_SAFE_CALL(cudaMemcpyAsync(state.viewCuda, staging, EyeGpuState::viewParamsFloats * sizeof(float), cudaMemcpyHostToDevice));
}

// Bitwise comparison: values computed the same way from the same camera are identical, NaNs included
// (operator== on floats would never match a NaN gaze, so every frame would silently fall back to mono)
template<typename T>
static bool sameBits(const T& a, const T& b) {
    static_assert(std::is_trivially_copyable_v<T>);
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

template<typename Scalar, int Rows, int Cols, int Options, int MaxRows, int MaxCols>
static bool sameBits(const Eigen::Matrix<Scalar, Rows, Cols, Options, MaxRows, MaxCols>& a,
                     const Eigen::Matrix<Scalar, Rows, Cols, Options, MaxRows, MaxCols>& b) {
    static_assert(Rows > 0 && Cols > 0, "fixed-size matrices only");
    return std::memcmp(a.data(), b.data(), sizeof(Scalar) * Rows * Cols) == 0;
}

bool sibr::EyeRenderParams::operator==(const EyeRenderParams& other) const {
    return sameBits(slot, other.slot) && sameBits(eyeIndex, other.eyeIndex)
        && sameBits(focalX, other.focalX) && sameBits(focalY, other.focalY)
        && sameBits(centerX, other.centerX) && sameBits(centerY, other.centerY)
        && sameBits(gazePos, other.gazePos)
        && sameBits(view, other.view) && sameBits(viewProj, other.viewProj) && sameBits(invRVP, other.invRVP)
        && sameBits(eyePos, other.eyePos)
        && sameBits(zNear, other.zNear) && sameBits(zFar, other.zFar);
}


Masks sibr::HTGaussianView::makeMasks(const uint32_t slot) const {
    return Masks{
        .visibility_mask = visibilityMask[slot],
        .visibility_mask_area_table = visibilityMaskAreaTable[slot],
        .fovea_mask_area_table = foveaMaskAreaTable
    };
}

BackgroundModel sibr::HTGaussianView::makeBackground() const {
    return BackgroundModel{
        .data = bgModelDataCuda,
        .type = backgroundModelType,
        .texture_resolution = backgroundTextureResolution
    };
}


void sibr::HTGaussianView::rasterizeMono(const EyeRenderParams& params, EyeGpuState& state) {
    uploadPose(state, params);

    // Map OpenGL buffer resource for use with CUDA
    float* imageCuda = nullptr;
    float* intermediateImageCuda = nullptr;
    size_t bytes;
    CUDA_SAFE_CALL(cudaGraphicsMapResources(1, &state.imageBufferCuda));
    CUDA_SAFE_CALL(cudaGraphicsResourceGetMappedPointer(
            reinterpret_cast<void**>(&imageCuda), &bytes, state.imageBufferCuda));

    // If peripheral blur is enabled, we need to allocate a secondary buffer
    if (blurPeriphery && modelType == RasterizerVariant::HTGS_FOVEATED) {
        intermediateImageCuda = reinterpret_cast<float*>(state.blurImageBuffer.resizeFunc(
            _resolution.x() * _resolution.y() * 3 * sizeof(float))
        );
    } else {
        intermediateImageCuda = imageCuda;
    }

    // Choose rasterizer and launch
    if (modelType == RasterizerVariant::HTGS) {
        CUDA_SAFE_CALL(htgs::rasterization::hybrid_blend::fast_inference(
            state.perPrimitiveBuffer.resizeFunc,
            state.perTileBuffer.resizeFunc,
            state.perInstanceBuffer.resizeFunc,
            (float3*)posCuda,
            (float3*)scaleCuda,
            (float4*)rotCuda,
            opacityCuda,
            (float3*)shs0Cuda,
            (float3*)shsRestCuda,
            (float4*)state.viewCuda,
            (float4*)state.projCuda,
            (float3*)state.camPosCuda,
            imageCuda,
            /*K=*/ 16,
            /*n_primitives=*/ count,
            /*active_sh_bases=*/ 15,
            /*total_sh_bases=*/ 15,           // max degree 3 = 15 bases (16-sh0)
            _resolution.x(), _resolution.y(),
            /*near_plane=*/ params.zNear,
            /*far_plane=*/ params.zFar,
            /*scale_modifier=*/ 1.0f,
            /*to_chw=*/ true
        ));
    } else if (modelType == RasterizerVariant::HTGS_FOVEATED) {
        CUDA_SAFE_CALL(vrhtgs::rasterization::inference(
            makeBuffers(state),
            (float3*)posCuda,
            (float3*)scaleCuda,
            (float4*)rotCuda,
            opacityCuda,
            (float3*)shs0Cuda,
            (float3*)shsRestCuda,
            makeBackground(),
            makePose(state, params),
            makeIntrinsics(params, _resolution),
            makeMasks(params.slot),
            intermediateImageCuda,
            imageCuda,
            /*K=*/ 16,
            /*n_primitives=*/ count,
            /*active_sh_bases=*/ 15,
            /*total_sh_bases=*/ 15,           // max degree 3 = 15 bases (16-sh0)
            /*near_plane=*/ params.zNear,
            /*far_plane=*/ params.zFar,
            /*scale_modifier=*/ 1.0f,
            /*to_chw=*/ true,
            blurPeriphery,
            antiAliasing
        ));
    } else {
        SIBR_ERR << "Unknown rasterizer variant selected!";
    }

    // Unmap OpenGL resource for use with OpenGL
    CUDA_SAFE_CALL(cudaGraphicsUnmapResources(1, &state.imageBufferCuda));
}


void sibr::HTGaussianView::rasterizeStereo(const EyeRenderParams& left, const EyeRenderParams& right) {
    EyeGpuState& stateLeft = eyeGpu[0];
    EyeGpuState& stateRight = eyeGpu[1];
    uploadPose(stateLeft, left);
    uploadPose(stateRight, right);

    // Map both eyes' OpenGL buffers for use with CUDA
    cudaGraphicsResource_t resources[2] = { stateLeft.imageBufferCuda, stateRight.imageBufferCuda };
    float* imageCuda[2] = { nullptr, nullptr };
    float* intermediateImageCuda[2] = { nullptr, nullptr };
    size_t bytes;
    CUDA_SAFE_CALL(cudaGraphicsMapResources(2, resources));
    for (int i = 0; i < 2; ++i) {
        CUDA_SAFE_CALL(cudaGraphicsResourceGetMappedPointer(
                reinterpret_cast<void**>(&imageCuda[i]), &bytes, resources[i]));
        // If peripheral blur is enabled, the eye needs a secondary buffer
        intermediateImageCuda[i] = blurPeriphery
            ? reinterpret_cast<float*>(eyeGpu[i].blurImageBuffer.resizeFunc(_resolution.x() * _resolution.y() * 3 * sizeof(float)))
            : imageCuda[i];
    }

    // Both eyes share the scalar parameters (near/far come from the same SIBR camera)
    CUDA_SAFE_CALL(vrhtgs::rasterization::inference_stereo(
        makeBuffers(stateLeft),
        makeBuffers(stateRight),
        (float3*)posCuda,
        (float3*)scaleCuda,
        (float4*)rotCuda,
        opacityCuda,
        (float3*)shs0Cuda,
        (float3*)shsRestCuda,
        makeBackground(),
        makePose(stateLeft, left),
        makePose(stateRight, right),
        makeIntrinsics(left, _resolution),
        makeIntrinsics(right, _resolution),
        makeMasks(0),
        makeMasks(1),
        intermediateImageCuda[0],
        imageCuda[0],
        intermediateImageCuda[1],
        imageCuda[1],
        /*K=*/ 16,
        /*n_primitives=*/ count,
        /*active_sh_bases=*/ 15,
        /*total_sh_bases=*/ 15,           // max degree 3 = 15 bases (16-sh0)
        /*near_plane=*/ left.zNear,
        /*far_plane=*/ left.zFar,
        /*scale_modifier=*/ 1.0f,
        /*to_chw=*/ true,
        blurPeriphery,
        antiAliasing
    ));

    CUDA_SAFE_CALL(cudaGraphicsUnmapResources(2, resources));
}


bool sibr::HTGaussianView::stereoApplies() const {
    return stereoRendering && modelType == RasterizerVariant::HTGS_FOVEATED && !gazeVisualization;
}


void sibr::HTGaussianView::configureStereo(const bool noStereo, const bool hookCalled) {
    stereoRendering = !noStereo;
    stereoHookCalled = hookCalled;
    stereoUnavailableReason = hookCalled ? "" : "Stereo rendering only applies in OpenXR mode (--rendering-mode 2). This mode renders mono.";
    if (noStereo) {
        SIBR_LOG << "Stereo rendering: off (--no-stereo)" << std::endl;
    } else if (hookCalled) {
        SIBR_LOG << "Stereo rendering: on for the VRHTGS rasterizer (HTGS renders mono; disable with --no-stereo)" << std::endl;
    } else {
        SIBR_LOG << "Stereo rendering: ignored, only applies in OpenXR mode (--rendering-mode 2); rendering mono" << std::endl;
    }
}


void sibr::HTGaussianView::releaseSecondEye() {
    if (eyeGpu[1].allocated) {
        SIBR_LOG << "Stereo rendering: freeing GPU state for the second eye (stereo not in use)" << std::endl;
        stereoReady[1].ready = false;
        freeEyeGpuState(eyeGpu[1]);
    }
}


void sibr::HTGaussianView::countStereoEyeImage(const bool fellBack) {
    ++stereoStatsTotal.stereoEyeImages;
    if (!fellBack) {
        return;
    }
    ++stereoStatsTotal.monoFallbacks;
    ++fallbacksSinceWarning;

    // Rate-limited: at most one warning every few seconds
    constexpr auto WARNING_INTERVAL = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();
    if (lastFallbackWarning.time_since_epoch().count() == 0 || now - lastFallbackWarning >= WARNING_INTERVAL) {
        SIBR_WRG << "Stereo rendering: " << fallbacksSinceWarning << " eye image(s) rendered mono instead of stereo"
                 << " since the last warning (the stereo image did not match the eye's camera or settings); "
                 << stereoStatsTotal.monoFallbacks << " of " << stereoStatsTotal.stereoEyeImages << " in total" << std::endl;
        lastFallbackWarning = now;
        fallbacksSinceWarning = 0;
    }
}


void sibr::HTGaussianView::onPrepareStereoFrame(const sibr::Camera& left, const sibr::Camera& right) {
    invalidateStereoImages();
    stereoAttempted[0] = stereoAttempted[1] = false;

    // Free the second eye's GPU state once stereo is no longer in use (setting off, HTGS rasterizer). Not on
    // the transient mono phases (gaze visualization, questionnaire), so it isn't re-allocated over and over.
    if (!stereoRendering || modelType != RasterizerVariant::HTGS_FOVEATED) {
        releaseSecondEye();
    }
    if (!stereoApplies() || eyeSlot(left) != 0 || eyeSlot(right) != 1) {
        return;
    }

    if (!eyeGpu[1].allocated) {
        SIBR_LOG << "Stereo rendering: allocating GPU state for the second eye" << std::endl;
        allocateEyeGpuState(eyeGpu[1]);
    }

    const EyeRenderParams params[2] = { computeEyeRenderParams(left), computeEyeRenderParams(right) };
    // The masks' recording records are written when each eye is rendered (as in mono), see updateVisibilityMask
    updateVisibilityMask(left, _resolution.x(), _resolution.y());
    updateVisibilityMask(right, _resolution.x(), _resolution.y());

    rasterizeStereo(params[0], params[1]);

    for (int i = 0; i < 2; ++i) {
        stereoReady[i].ready = true;
        stereoReady[i].params = params[i];
        stereoReady[i].modelType = modelType;
        stereoReady[i].antiAliasing = antiAliasing;
        stereoReady[i].blurPeriphery = blurPeriphery;
        stereoAttempted[i] = true;
    }

    if (cudaPeekAtLastError() != cudaSuccess) {
        SIBR_ERR << "A CUDA error occurred during stereo rendering:" << cudaGetErrorString(cudaGetLastError()) << ". Please rerun in Debug to find the exact line!";
    }
}


void sibr::HTGaussianView::onRenderIBR(sibr::IRenderTarget& dst, const sibr::Camera& eye) {
    // Remember the eye pose (world space) so controller input can be made head-relative
    eyeWorldPosition[eyeSlot(eye)] = eye.position();
    eyeWorldForward[eyeSlot(eye)] = eye.dir();
    eyeWorldUp[eyeSlot(eye)] = eye.up();
    if (eye.viewIndex() == 1) {
        stereoEyesSeen = true;
    }

    const EyeRenderParams params = computeEyeRenderParams(eye);
    EyeGpuState& monoState = eyeGpu[0];

    // If in gaze visualization mode, skip main rendering & render gaze position instead
    if (gazeVisualization) {
        float* imageCuda = nullptr;
        size_t bytes;

        CUDA_SAFE_CALL(cudaGraphicsMapResources(1, &monoState.imageBufferCuda));
        CUDA_SAFE_CALL(cudaGraphicsResourceGetMappedPointer(
                reinterpret_cast<void**>(&imageCuda), &bytes, monoState.imageBufferCuda));

        CUDA_SAFE_CALL(vrhtgs::visualization::clear_image(
            imageCuda,
            _resolution.x(),
            _resolution.y()
        ));
        CUDA_SAFE_CALL(vrhtgs::visualization::visualize_gaze_position(
            imageCuda,
            _resolution.x(),
            _resolution.y(),
            40,  // radius in pixels
            (float2*)params.gazePos,
            make_float3(1.0f, 0.0f, 0.0f),  // color
            vrhtgs::visualization::GazeVisualizationType::CIRCLE,
            /*to_chw=*/ true
        ));

        // Copy image contents to framebuffer
        _copyRenderer->process(monoState.imageBuffer, dst, _resolution.x(), _resolution.y());
        CUDA_SAFE_CALL(cudaGraphicsUnmapResources(1, &monoState.imageBufferCuda));

        return;
    }

    // Update visibility mask if needed; its record (if it changed here or in the stereo hook) goes before this
    // eye's frame records, as in mono rendering
    updateVisibilityMask(eye, _resolution.x(), _resolution.y());
    writeVisibilityMaskRecord(params.slot);

    // Show this eye's stereo image if the stereo hook rendered one for exactly this eye and these
    // settings; otherwise render mono (never show a stale image)
    StereoReadyImage& ready = stereoReady[params.slot];
    const bool useStereoImage = ready.ready && ready.params == params && ready.modelType == modelType
                                && ready.antiAliasing == antiAliasing && ready.blurPeriphery == blurPeriphery;
    ready.ready = false;
    if (stereoAttempted[params.slot]) {
        stereoAttempted[params.slot] = false;
        countStereoEyeImage(!useStereoImage);
    }
    EyeGpuState& shownState = useStereoImage ? eyeGpu[params.slot] : monoState;
    if (!useStereoImage) {
        rasterizeMono(params, monoState);
    }

    // Record frame if recording is enabled
    if (recordingWriter) {
        // Timing of the headset frame this eye belongs to, once before its first eye record
        if (pendingFrameBegin) {
            recordingWriter->writeFrameBegin(*pendingFrameBegin);
            pendingFrameBegin.reset();
        }
        if (modelType == RasterizerVariant::HTGS) {
            htgs::recording::FrameRenderHTGSRecord record;
            std::memcpy(record.view, params.view.data(), sizeof(record.view));
            std::memcpy(record.proj, params.viewProj.data(), sizeof(record.proj));
            std::memcpy(record.camPos, params.eyePos.data(), sizeof(record.camPos));
            record.resolutionX = _resolution.x();
            record.resolutionY = _resolution.y();
            record.nearPlane = params.zNear;
            record.farPlane = params.zFar;
            record.gazeX = params.gazePos[0];
            record.gazeY = params.gazePos[1];
            record.eyeIndex = params.eyeIndex;
            recordingWriter->writeFrameRenderHTGS(record);
        } else {
            htgs::recording::FrameRenderHTGSFoveatedRecord record;
            std::memcpy(record.view, params.view.data(), sizeof(record.view));
            std::memcpy(record.proj, params.viewProj.data(), sizeof(record.proj));
            std::memcpy(record.invRVP, params.invRVP.data(), sizeof(record.invRVP));
            std::memcpy(record.camPos, params.eyePos.data(), sizeof(record.camPos));
            record.resolutionX = _resolution.x();
            record.resolutionY = _resolution.y();
            record.focalX = params.focalX;
            record.focalY = params.focalY;
            record.centerX = params.centerX;
            record.centerY = params.centerY;
            record.nearPlane = params.zNear;
            record.farPlane = params.zFar;
            record.gazeX = params.gazePos[0];
            record.gazeY = params.gazePos[1];
            record.eyeIndex = params.eyeIndex;
            recordingWriter->writeFrameRenderHTGSFoveated(record);
        }
    }

    // Copy image contents to framebuffer
    _copyRenderer->process(shownState.imageBuffer, dst, _resolution.x(), _resolution.y());

    if (cudaPeekAtLastError() != cudaSuccess) {
        SIBR_ERR << "A CUDA error occurred during rendering:" << cudaGetErrorString(cudaGetLastError()) << ". Please rerun in Debug to find the exact line!";
    }
}

void sibr::HTGaussianView::onUpdate(Input & input) {}

void sibr::HTGaussianView::onGUI() {
    // Generate and update UI elements
    const std::string guiName = "3D Gaussians";
    if (ImGui::Begin(guiName.c_str())) {
        ImGui::TextWrapped("%s", conditionInfo.c_str());

        ImGui::DragFloat("Height Adjustment", &heightAdjustment, 0.001f, -100.0f, 100.0f, "%.3f m");

        ImGui::Checkbox("Anti-Aliasing", &antiAliasing);
        ImGui::Checkbox("Blur Periphery", &blurPeriphery);
        if (ImGui::BeginCombo("Rasterizer Variant",
                (modelType == RasterizerVariant::HTGS ? "HTGS" : "HTGS_FOVEATED")
        )) {
            if (ImGui::Selectable("HTGS")) {
                modelType = RasterizerVariant::HTGS;
            }

            if (ImGui::Selectable("HTGS_FOVEATED")) {
                modelType = RasterizerVariant::HTGS_FOVEATED;
            }

            ImGui::EndCombo();
        }

        // Stereo: both eyes in one VR-HTGS call (starts from --no-stereo; mono where it can't apply)
        const bool stereoUnavailable = !stereoHookCalled;
        const bool stereoDisabled = stereoUnavailable || modelType == RasterizerVariant::HTGS;
        ImGui::BeginDisabled(stereoDisabled);
        ImGui::Checkbox("Stereo Rendering", &stereoRendering);
        ImGui::EndDisabled();
        if (stereoStatsTotal.monoFallbacks > 0) {
            // Eye images that should have been stereo but were rendered mono (see the console warnings)
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), "%llu of %llu eye images fell back to mono",
                               static_cast<unsigned long long>(stereoStatsTotal.monoFallbacks),
                               static_cast<unsigned long long>(stereoStatsTotal.stereoEyeImages));
        }
        if (stereoUnavailable) {
            ImGui::TextWrapped("%s", stereoUnavailableReason.c_str());
        } else if (modelType == RasterizerVariant::HTGS) {
            ImGui::TextWrapped("The HTGS rasterizer always renders mono.");
        } else if (stereoRendering && gazeVisualization) {
            ImGui::TextWrapped("Mono while the gaze visualization is shown.");
        }

        if (calibrationMode) {
            ImGui::Separator();
            ImGui::Text("Calibration Mode");
            ImGui::TextWrapped("Stick: %s  (Y/B: next mode)", calibrationControlDescription(calibrationControl));

            if (ImGui::DragFloat("Model Scale", &calibrationData.modelScale, 0.001f, 0.001f, 100.0f)) {
                uploadScaledModel();
            }

            sibr::Vector3f translation = sceneTransform.block<3,1>(0,3);
            if (ImGui::DragFloat3("Starting Position", translation.data(), 0.0005f, -100.0f, 100.0f)) {
                sceneTransform.block<3,1>(0,3) = translation;
            }

            // Yaw = heading of the model's z axis around the world's vertical axis. Read with atan2
            // (stable over the full circle, unlike Eigen's eulerAngles) and applied as a relative
            // rotation, so any pitch/roll in the loaded pose is kept.
            const sibr::Matrix3f rotation = sceneTransform.block<3,3>(0,0);
            const float currentYaw = std::atan2(rotation(0, 2), rotation(2, 2));
            float yaw = currentYaw;
            if (ImGui::DragFloat("Starting Yaw", &yaw, 0.001f, -3.14159f, 3.14159f)) {
                sceneTransform.block<3,3>(0,0) =
                    Eigen::AngleAxisf(yaw - currentYaw, sibr::Vector3f::UnitY()).toRotationMatrix() * rotation;
            }
        } else {
            ImGui::Separator();
            ImGui::Text("Locomotion");
            ImGui::TextWrapped("Left stick: move / strafe. Right stick: turn (x), up / down (y). Y/B: reset.");
            ImGui::DragFloat("Move Speed", &locomotionSpeed, 0.01f, 0.0f, 20.0f, "%.2f m/s");
            ImGui::DragFloat("Turn Speed", &locomotionTurnSpeed, 0.5f, 0.0f, 360.0f, "%.0f deg/s");
            if (ImGui::Button("Reset Position")) {
                resetLocomotion();
            }
        }
    }
    ImGui::End();
}

const char* sibr::HTGaussianView::calibrationControlDescription(CalibrationControl control) {
    switch (control) {
        case CalibrationControl::SCALE:               return "Scale (y), around the floor point below the head";
        case CalibrationControl::YAW:                 return "Turn yourself (x), around the head";
        case CalibrationControl::POSITION_HORIZONTAL: return "Move yourself horizontally (x / y), relative to the head direction";
        case CalibrationControl::HEIGHT:              return "Move yourself up / down (y)";
        default:                                      return "?";
    }
}

void sibr::HTGaussianView::uploadScaledModel() {
    if (calibrationData.positions.size() != static_cast<size_t>(count) ||
        calibrationData.scales.size() != static_cast<size_t>(count)) {
        return;  // CPU copies only exist in calibration mode
    }
    invalidateStereoImages();

    const float factor = calibrationData.modelScale / calibrationData.baseModelScale;
    std::vector<Pos> scaledPositions = calibrationData.positions;
    std::vector<Scale> scaledScales = calibrationData.scales;
    for (auto& s : scaledScales) {
        s.scale[0] *= factor;
        s.scale[1] *= factor;
        s.scale[2] *= factor;
    }
    for (auto& p : scaledPositions) {
        p *= factor;
    }

    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(scaleCuda, scaledScales.data(), sizeof(Scale) * count, cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL_ALWAYS(cudaMemcpy(posCuda, scaledPositions.data(), sizeof(Pos) * count, cudaMemcpyHostToDevice));
}

float sibr::HTGaussianView::inputDeltaTime() {
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (lastInputTime.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - lastInputTime).count();
    }
    lastInputTime = now;
    // Don't jump after a stall (model loading, a hitch, the first call)
    return std::clamp(dt, 0.0f, 0.1f);
}

sibr::HTGaussianView::HeadFrame sibr::HTGaussianView::headFrame() const {
    HeadFrame frame;
    frame.position = headWorldPosition();

    sibr::Vector3f forward = eyeWorldForward[0];
    sibr::Vector3f up = eyeWorldUp[0];
    if (stereoEyesSeen) {
        forward += eyeWorldForward[1];
        up += eyeWorldUp[1];
    }

    // The world's vertical axis is y in both the OpenXR (+y up) and the Y-inverted (+y down)
    // convention; which way is "up" follows from the head's up vector.
    frame.up = sibr::Vector3f(0.0f, up.y() >= 0.0f ? 1.0f : -1.0f, 0.0f);

    sibr::Vector3f horizontal = forward - forward.dot(frame.up) * frame.up;
    if (horizontal.norm() < 0.1f * forward.norm()) {
        // Looking almost straight up or down: the head's up vector points (back)wards instead
        const sibr::Vector3f upHorizontal = up - up.dot(frame.up) * frame.up;
        horizontal = (forward.dot(frame.up) < 0.0f ? 1.0f : -1.0f) * upHorizontal;
    }
    if (horizontal.norm() < 1e-6f) {
        horizontal = sibr::Vector3f(0.0f, 0.0f, frame.up.y() < 0.0f ? 1.0f : -1.0f);
    }
    frame.forward = horizontal.normalized();
    frame.right = frame.forward.cross(frame.up);
    return frame;
}

sibr::Matrix4f sibr::HTGaussianView::locomotionTransform() const {
    sibr::Matrix4f transform = sibr::Matrix4f::Identity();
    transform.block<3,3>(0,0) = Eigen::AngleAxisf(locomotionYaw, sibr::Vector3f::UnitY()).toRotationMatrix();
    transform.block<3,1>(0,3) = locomotionTranslation;
    return transform;
}

static float applyDeadzone(const float value, const float deadzone = 0.15f) {
    const float magnitude = std::abs(value);
    if (magnitude <= deadzone) {
        return 0.0f;
    }
    return std::copysign(std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone)), value);
}

void sibr::HTGaussianView::handleLocomotionInput(const sibr::ControllerState& controller) {
    const float dt = inputDeltaTime();

    if (controller.backState) {
        resetLocomotion();
        return;
    }

    const float moveForward = applyDeadzone(controller.leftStickY);
    const float moveRight = applyDeadzone(controller.leftStickX);
    const float turnRight = applyDeadzone(controller.rightStickX);
    const float moveUp = applyDeadzone(controller.rightStickY);
    if (moveForward == 0.0f && moveRight == 0.0f && turnRight == 0.0f && moveUp == 0.0f) {
        return;
    }

    const HeadFrame head = headFrame();

    // Moving the user by d is moving the scene by -d
    const sibr::Vector3f userMotion = (moveForward * head.forward + moveRight * head.right + moveUp * head.up)
                                      * locomotionSpeed * dt;
    locomotionTranslation -= userMotion;

    // Turning the user right by angle a (forward towards right, i.e. around -up) is turning the
    // scene by -a around -up = a around up, pivoting at the head so the user stays in place.
    if (turnRight != 0.0f) {
        const float angle = turnRight * locomotionTurnSpeed * (std::numbers::pi_v<float> / 180.0f) * dt;
        const float yawAroundY = angle * head.up.y();  // head.up is +-UnitY
        const sibr::Matrix3f rotation = Eigen::AngleAxisf(yawAroundY, sibr::Vector3f::UnitY()).toRotationMatrix();
        locomotionYaw += yawAroundY;
        locomotionTranslation = head.position + rotation * (locomotionTranslation - head.position);
    }
}

void sibr::HTGaussianView::handleCalibrationInput(const sibr::ControllerState& controller) {
    if (!calibrationMode) {
        return;  // The CPU copies of the model needed for rescaling only exist in calibration mode
    }

    const float dt = inputDeltaTime();

    if (controller.backState) {
        calibrationControl = static_cast<CalibrationControl>(
            (static_cast<int>(calibrationControl) + 1) % static_cast<int>(CalibrationControl::COUNT));
        SIBR_LOG << "Calibration: stick now adjusts: " << calibrationControlDescription(calibrationControl) << std::endl;
        return;
    }

    const float stickX = applyDeadzone(controller.stickXState);
    const float stickY = applyDeadzone(controller.stickYState);
    if (stickX == 0.0f && stickY == 0.0f) {
        return;
    }

    // All adjustments are made in world space (i.e. pre-multiplied to sceneTransform), relative to
    // the user's head, so they behave the same whichever way the user is facing.
    const HeadFrame head = headFrame();
    const sibr::Vector3f floorPoint = head.position - head.position.dot(head.up) * head.up;
    constexpr float MOVE_SPEED = 0.5f;   // m/s at full deflection
    constexpr float TURN_SPEED = 0.5f;   // rad/s at full deflection
    constexpr float SCALE_SPEED = 0.5f;  // log-scale units/s at full deflection

    sibr::Vector3f translation = sceneTransform.block<3,1>(0,3);
    switch (calibrationControl) {
        case CalibrationControl::SCALE: {
            if (stickY == 0.0f) break;
            const float oldScale = calibrationData.modelScale;
            calibrationData.modelScale = std::clamp(oldScale * std::exp(stickY * SCALE_SPEED * dt), 0.001f, 100.0f);
            // Keep the scene point on the floor below the head in place
            const float ratio = calibrationData.modelScale / oldScale;
            translation = floorPoint + ratio * (translation - floorPoint);
            uploadScaledModel();
            break;
        }
        case CalibrationControl::YAW: {
            if (stickX == 0.0f) break;
            // Feels like turning yourself: stick right turns the user right, i.e. the scene left
            // (what is to the right comes to the front), around the head
            const float yawAroundY = stickX * TURN_SPEED * dt * head.up.y();
            const sibr::Matrix3f rotation = Eigen::AngleAxisf(yawAroundY, sibr::Vector3f::UnitY()).toRotationMatrix();
            sceneTransform.block<3,3>(0,0) = rotation * sceneTransform.block<3,3>(0,0);
            translation = floorPoint + rotation * (translation - floorPoint);
            break;
        }
        case CalibrationControl::POSITION_HORIZONTAL:
            // Feels like moving yourself: stick forward moves the user forward (the scene towards
            // the user), stick right moves the user right (the scene to the left)
            translation -= (stickY * head.forward + stickX * head.right) * MOVE_SPEED * dt;
            break;
        case CalibrationControl::HEIGHT:
            // Stick up moves the user up (the scene down)
            translation -= stickY * head.up * MOVE_SPEED * dt;
            break;
        default:
            break;
    }
    sceneTransform.block<3,1>(0,3) = translation;
}


void sibr::HTGaussianView::enableRecording(const std::string& filename) {
    if (recordingWriter) {
        SIBR_WRG << "Recording already enabled, disabling previous recording" << std::endl;
        disableRecording();
    }

    try {
        recordingWriter = std::make_unique<htgs::recording::RecordingWriter>(filename);

        // Record initial scene state if available
        if (currentConditionIndex >= 0 && !currentModelPath.empty()) {
            recordingWriter->writeSceneChange(
                currentConditionIndex,
                static_cast<htgs::recording::RasterizerVariant>(modelType),
                currentModelPath,
                currentSceneScale,
                antiAliasing,
                blurPeriphery
            );
        }

        SIBR_LOG << "Recording enabled to file: " << filename << std::endl;
    } catch (const std::exception& e) {
        SIBR_ERR << "Failed to enable recording: " << e.what() << std::endl;
        recordingWriter.reset();
    }
}

void sibr::HTGaussianView::disableRecording() {
    if (recordingWriter) {
        recordingWriter.reset();
        SIBR_LOG << "Recording disabled" << std::endl;
    }
}


sibr::HTGaussianView::~HTGaussianView() {
    // Cleanup
    cudaFree(posCuda);
    cudaFree(rotCuda);
    cudaFree(scaleCuda);
    cudaFree(opacityCuda);
    cudaFree(shs0Cuda);
    cudaFree(shsRestCuda);
    cudaFree(bgModelDataCuda);

    for (EyeGpuState& state : eyeGpu) {
        freeEyeGpuState(state);
    }
    cudaFreeHost(viewParamsHost);

    delete _copyRenderer;
}
