#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <thread>

#include "boost/asio.hpp"
#include "imgui_internal.h"
#include <openxr/openxr.h>

#include "core/graphics/GUI.hpp"
#include "core/openxr/ControllerState.hpp"
#include "VRHTGSCudaBackend/rasterization/include/inference.h"
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

            dst.clear();
            dst.bind();

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
    } else {
        SIBR_LOG << "No background model data found\n";
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


sibr::HTGaussianView::HTGaussianView(const sibr::BasicIBRScene::Ptr& ibrScene,
        uint32_t renderW, uint32_t renderH, const char* file,
        const float scale, int device, bool calibrationMode) :
    _scene(ibrScene),
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

    // Set up resize functions for our buffers
    perPrimitiveBuffer.resizeFunc = getBufferGrowFunction(&perPrimitiveBuffer);
    perTileBuffer.resizeFunc = getBufferGrowFunction(&perTileBuffer);
    perSubtileBuffer.resizeFunc = getBufferGrowFunction(&perSubtileBuffer);
    // Since this is the only buffer that changes while rendering the same scene,
    //   we overallocate by 1.25x to reduce the number of necessary resizes.
    perInstanceBuffer.resizeFunc = getBufferGrowFunction(&perInstanceBuffer, 1.25);
    // Special buffer to contain the intermediate image for blurring
    blurImageBuffer.resizeFunc = getBufferResizeFunction(&blurImageBuffer, 1.1, 0.75);

    // Initialize copy renderer
    _copyRenderer = new BufferCopyRenderer();
    _copyRenderer->flip() = true;
    _copyRenderer->width() = _resolution.x();
    _copyRenderer->height() = _resolution.y();

    // Load model and set up rendering buffers
    loadModel(file, scale);
    createImageBuffer();
    initializeFoveaMask();

    // Allocate GPU memory for view parameters
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&viewCuda), sizeof(sibr::Matrix4f)));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&projCuda), sizeof(sibr::Matrix4f)));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&invRVPCuda), sizeof(sibr::Matrix4f)));
    CUDA_SAFE_CALL_ALWAYS(cudaMalloc(reinterpret_cast<void**>(&camPosCuda), 3 * sizeof(float)));
}


void sibr::HTGaussianView::setResolution(const Vector2i &size) {
    if (size != getResolution()) {
        SIBR_LOG << "Set resolution => " << size << std::endl;
        ViewBase::setResolution(size);
        destroyImageBuffer();
        createImageBuffer();

        _copyRenderer->width() = _resolution.x();
        _copyRenderer->height() = _resolution.y();
    }
}

void sibr::HTGaussianView::setScene(const sibr::BasicIBRScene::Ptr& newScene) {
    _scene = newScene;
}

void sibr::HTGaussianView::loadModel(const char* file, float modelScale) {
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


void sibr::HTGaussianView::createImageBuffer() {
    // Create GL buffer ready for CUDA/GL interop
    glCreateBuffers(1, &imageBuffer);
    glCreateBuffers(1, &intermediateImageBuffer);
    glNamedBufferStorage(imageBuffer, _resolution.x() * _resolution.y() * 3 * sizeof(float), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glNamedBufferStorage(intermediateImageBuffer, _resolution.x() * _resolution.y() * 3 * sizeof(float), nullptr, GL_DYNAMIC_STORAGE_BIT);

    if (cudaPeekAtLastError() != cudaSuccess) {
        SIBR_ERR << "A CUDA error occurred in setup:" << cudaGetErrorString(cudaGetLastError()) << ". Please rerun in Debug to find the exact line!";
    }
    cudaGraphicsGLRegisterBuffer(&imageBufferCuda, imageBuffer, cudaGraphicsRegisterFlagsWriteDiscard);
    if (cudaPeekAtLastError() != cudaSuccess) {
        SIBR_ERR << "A CUDA error occurred in setup:" << cudaGetErrorString(cudaGetLastError()) << ". Please rerun in Debug to find the exact line!";
    }
}

void sibr::HTGaussianView::destroyImageBuffer() {
    cudaGraphicsUnregisterResource(imageBufferCuda);
    glDeleteBuffers(1, &imageBuffer);
    imageBuffer = 0;
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

    // Pack mask into uint32_t array
    std::vector<uint32_t> visibilityMaskPacked(
        (widthTiles * heightTiles + sizeof(uint32_t) - 1) / sizeof(uint32_t),
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

    // Record visibility mask update if recording is enabled and mask has changed
    if (recordingWriter) {
        recordingWriter->writeVisibilityMaskUpdate(widthTiles, heightTiles, visibilityMaskPacked.data());
    }
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


void sibr::HTGaussianView::onRenderIBR(sibr::IRenderTarget& dst, const sibr::Camera& eye) {
    // Remember the eye pose (world space) so controller input can be made head-relative
    eyeWorldPosition[eyeSlot(eye)] = eye.position();
    eyeWorldForward[eyeSlot(eye)] = eye.dir();
    eyeWorldUp[eyeSlot(eye)] = eye.up();
    if (eye.viewIndex() == 1) {
        stereoEyesSeen = true;
    }

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
    float gazePosX = -gaze_x * focalX + centerX;
    float gazePosY = gaze_y * focalY + centerY;
    float gazePos[2] = {gazePosX, gazePosY};

    // If in gaze visualization mode, skip main rendering & render gaze position instead
    if (gazeVisualization) {
        float* imageCuda = nullptr;
        size_t bytes;

        CUDA_SAFE_CALL(cudaGraphicsMapResources(1, &imageBufferCuda));
        CUDA_SAFE_CALL(cudaGraphicsResourceGetMappedPointer(
                reinterpret_cast<void**>(&imageCuda), &bytes, imageBufferCuda));

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
            (float2*)gazePos,
            make_float3(1.0f, 0.0f, 0.0f),  // color
            vrhtgs::visualization::GazeVisualizationType::CIRCLE,
            /*to_chw=*/ true
        ));

        // Copy image contents to framebuffer
        _copyRenderer->process(imageBuffer, dst, _resolution.x(), _resolution.y());
        CUDA_SAFE_CALL(cudaGraphicsUnmapResources(1, &imageBufferCuda));

        return;
    }

    // Get remaining view parameters for main render path
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
    auto mvp = sibr::Matrix4f(projMat * viewMat);
    auto invRVP = sibr::Matrix4f(c2wRot * invProjMat);
    viewMat.transposeInPlace();
    mvp.transposeInPlace();
    invRVP.transposeInPlace();

    // Get camera position & transform
    sibr::Vector4f eyePosRaw = sibr::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
    eyePosRaw.head<3>() = eye.position();
    sibr::Vector4f eyePosTransformed = sceneTransform.inverse() * eyePosRaw;
    sibr::Vector3f eyePos = eyePosTransformed.head<3>() / eyePosTransformed.w();

    // Update visibility mask if needed
    updateVisibilityMask(eye, _resolution.x(), _resolution.y());

    // Copy frame-dependent data to GPU
    CUDA_SAFE_CALL(cudaMemcpy(viewCuda, viewMat.data(), sizeof(sibr::Matrix4f), cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL(cudaMemcpy(projCuda, mvp.data(), sizeof(sibr::Matrix4f), cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL(cudaMemcpy(invRVPCuda, invRVP.data(), sizeof(sibr::Matrix4f), cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL(cudaMemcpy(camPosCuda, &eyePos, sizeof(float) * 3, cudaMemcpyHostToDevice));

    // Map OpenGL buffer resource for use with CUDA
    float* imageCuda = nullptr;
    float* intermediateImageCuda = nullptr;
    size_t bytes;
    CUDA_SAFE_CALL(cudaGraphicsMapResources(1, &imageBufferCuda));
    CUDA_SAFE_CALL(cudaGraphicsResourceGetMappedPointer(
            reinterpret_cast<void**>(&imageCuda), &bytes, imageBufferCuda));

    // If peripheral blur is enabled, we need to allocate a secondary buffer
    if (blurPeriphery && modelType == RasterizerVariant::HTGS_FOVEATED) {
        intermediateImageCuda = reinterpret_cast<float*>(blurImageBuffer.resizeFunc(
            _resolution.x() * _resolution.y() * 3 * sizeof(float))
        );
    } else {
        intermediateImageCuda = imageCuda;
    }

    // Choose rasterizer and launch
    if (modelType == RasterizerVariant::HTGS) {
        CUDA_SAFE_CALL(htgs::rasterization::hybrid_blend::fast_inference(
            perPrimitiveBuffer.resizeFunc,
            perTileBuffer.resizeFunc,
            perInstanceBuffer.resizeFunc,
            (float3*)posCuda,
            (float3*)scaleCuda,
            (float4*)rotCuda,
            opacityCuda,
            (float3*)shs0Cuda,
            (float3*)shsRestCuda,
            (float4*)viewCuda,
            (float4*)projCuda,
            (float3*)camPosCuda,
            imageCuda,
            /*K=*/ 16,
            /*n_primitives=*/ count,
            /*active_sh_bases=*/ 15,
            /*total_sh_bases=*/ 15,           // max degree 3 = 15 bases (16-sh0)
            _resolution.x(), _resolution.y(),
            /*near_plane=*/ near,
            /*far_plane=*/ far,
            /*scale_modifier=*/ 1.0f,
            /*to_chw=*/ true
        ));
    } else if (modelType == RasterizerVariant::HTGS_FOVEATED) {
        const Buffers buffers = {
            .per_primitive_buffers_func = perPrimitiveBuffer.resizeFunc,
            .per_tile_buffers_func = perTileBuffer.resizeFunc,
            .per_subtile_buffers_func = perSubtileBuffer.resizeFunc,
            .per_instance_buffers_func = perInstanceBuffer.resizeFunc
        };
        const BackgroundModel bgModelData = {
            .data = bgModelDataCuda,
            .type = backgroundModelType,
            .texture_resolution = backgroundTextureResolution
        };
        const Pose pose = {
            .M = (float4*)viewCuda,
            .VPM = (float4*)projCuda,
            .VPR_inv = (float4*)invRVPCuda,
            .cam_position = (float3*)camPosCuda,
            .gaze_position = (float2*)gazePos
        };
        const Intrinsics intrinsics = {
            .width = _resolution.x(),
            .height = _resolution.y(),
            .focal_x = focalX,
            .focal_y = focalY,
            .center_x = centerX,
            .center_y = centerY
        };
        const Masks masks = {
            .visibility_mask = visibilityMask[eyeSlot(eye)],
            .visibility_mask_area_table = visibilityMaskAreaTable[eyeSlot(eye)],
            .fovea_mask_area_table = foveaMaskAreaTable
        };

        CUDA_SAFE_CALL(vrhtgs::rasterization::inference(
            buffers,
            (float3*)posCuda,
            (float3*)scaleCuda,
            (float4*)rotCuda,
            opacityCuda,
            (float3*)shs0Cuda,
            (float3*)shsRestCuda,
            bgModelData,
            pose,
            intrinsics,
            masks,
            intermediateImageCuda,
            imageCuda,
            /*K=*/ 16,
            /*n_primitives=*/ count,
            /*active_sh_bases=*/ 15,
            /*total_sh_bases=*/ 15,           // max degree 3 = 15 bases (16-sh0)
            /*near_plane=*/ near,
            /*far_plane=*/ far,
            /*scale_modifier=*/ 1.0f,
            /*to_chw=*/ true,
            blurPeriphery,
            antiAliasing
        ));
    } else {
        SIBR_ERR << "Unknown rasterizer variant selected!";
    }

    // Unmap OpenGL resource for use with OpenGL
    CUDA_SAFE_CALL(cudaGraphicsUnmapResources(1, &imageBufferCuda));

    // Record frame if recording is enabled
    if (recordingWriter) {
        if (modelType == RasterizerVariant::HTGS) {
            htgs::recording::FrameRenderHTGSRecord record;
            std::memcpy(record.view, viewMat.data(), sizeof(record.view));
            std::memcpy(record.proj, mvp.data(), sizeof(record.proj));
            std::memcpy(record.camPos, &eyePos, sizeof(record.camPos));
            record.resolutionX = _resolution.x();
            record.resolutionY = _resolution.y();
            record.nearPlane = near;
            record.farPlane = far;
            record.gazeX = gazePosX;
            record.gazeY = gazePosY;
            record.eyeIndex = eye.viewIndex();
            recordingWriter->writeFrameRenderHTGS(record);
        } else {
            htgs::recording::FrameRenderHTGSFoveatedRecord record;
            std::memcpy(record.view, viewMat.data(), sizeof(record.view));
            std::memcpy(record.proj, mvp.data(), sizeof(record.proj));
            std::memcpy(record.invRVP, invRVP.data(), sizeof(record.invRVP));
            std::memcpy(record.camPos, &eyePos, sizeof(record.camPos));
            record.resolutionX = _resolution.x();
            record.resolutionY = _resolution.y();
            record.focalX = focalX;
            record.focalY = focalY;
            record.centerX = centerX;
            record.centerY = centerY;
            record.nearPlane = near;
            record.farPlane = far;
            record.gazeX = gazePosX;
            record.gazeY = gazePosY;
            record.eyeIndex = eye.viewIndex();
            recordingWriter->writeFrameRenderHTGSFoveated(record);
        }
    }

    // Copy image contents to framebuffer
    _copyRenderer->process(imageBuffer, dst, _resolution.x(), _resolution.y());

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

    cudaFree(viewCuda);
    cudaFree(projCuda);
    cudaFree(invRVPCuda);
    cudaFree(camPosCuda);

    cudaGraphicsUnregisterResource(imageBufferCuda);
    glDeleteBuffers(1, &imageBuffer);

    delete _copyRenderer;
}
