# pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "core/system/Matrix.hpp"
#include "core/system/Vector.hpp"


namespace htgs::recording {

enum class RasterizerVariant : uint8_t {
    HTGS = 0,
    HTGS_FOVEATED = 1,
};

enum class RecordType : uint8_t {
    SCENE_CHANGE = 0,
    FOVEA_MASK_UPDATE = 1,
    VISIBILITY_MASK_UPDATE = 2,
    FRAME_RENDER = 3
};

#pragma pack(push, 1)
struct SceneChangeRecord {
    int16_t conditionIndex;
    RasterizerVariant modelType;
    // std::string modelPath;
    char modelPath[260]; // Using Windows MAX_PATH to ensure fixed size
    bool antiAliasing;
    bool blurPeriphery;
    float sceneScale;
    uint8_t peripheryInterpolationMode;  // 0 = NEAREST, 1 = LINEAR
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<SceneChangeRecord>, "SceneChangeRecord must be trivially copyable");

#pragma pack(push, 1)
struct FoveaMaskUpdateRecord {
    uint16_t widthTiles;
    uint16_t heightTiles;
    // Followed by ((width + 1) * (height + 1)) uint32_t values
    // -> We only store the summed area table, since the raw mask is not needed for replay
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<FoveaMaskUpdateRecord>, "FoveaMaskUpdateRecord must be trivially copyable");

#pragma pack(push, 1)
struct VisibilityMaskUpdateRecord {
    uint16_t widthTiles;
    uint16_t heightTiles;
    // Followed by ((widthTiles * heightTiles + sizeof(uint32_t) - 1) / sizeof(uint32_t) / 8) uint32_t values
    // -> We only store the packed raw mask, the summed area table can be recomputed at replay time
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<VisibilityMaskUpdateRecord>, "VisibilityMaskUpdateRecord must be trivially copyable");

#pragma pack(push, 1)
struct FrameRenderHTGSRecord {
    float view[16];  // sibr::Matrix4f view;
    float proj[16];  // sibr::Matrix4f proj;
    float camPos[3];  // sibr::Vector3f camPos;
    uint32_t resolutionX;
    uint32_t resolutionY;
    float nearPlane;
    float farPlane;
    float gazeX;
    float gazeY;
    uint8_t eyeIndex;  // 0 = left, 1 = right
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<FrameRenderHTGSRecord>, "FrameRenderHTGSRecord must be trivially copyable");

#pragma pack(push, 1)
struct FrameRenderHTGSFoveatedRecord {
    float view[16];  // sibr::Matrix4f view;
    float proj[16];  // sibr::Matrix4f proj;
    float invRVP[16];  // sibr::Matrix4f invRVP;
    float camPos[3];  // sibr::Vector3f camPos;
    uint32_t resolutionX;
    uint32_t resolutionY;
    float focalX;
    float focalY;
    float centerX;
    float centerY;
    float nearPlane;
    float farPlane;
    float gazeX;
    float gazeY;
    uint8_t eyeIndex;  // 0 = left, 1 = right
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<FrameRenderHTGSFoveatedRecord>, "FrameRenderHTGSFoveatedRecord must be trivially copyable");


class RecordingWriter {
    public:
        RecordingWriter(const std::string& filename);
        ~RecordingWriter();

        void writeSceneChange(int conditionIndex, RasterizerVariant modelType,
                              const std::string& modelPath, float sceneScale,
                              bool antiAliasing, bool blurPeriphery);

        void writeFoveaMaskUpdate(int widthTiles, int heightTiles,
                                  const uint32_t* summedAreaTable);

        void writeVisibilityMaskUpdate(int widthTiles, int heightTiles,
                                   const uint32_t* packedMask);

        void writeFrameRenderHTGS(const FrameRenderHTGSRecord& record);

        void writeFrameRenderHTGSFoveated(const FrameRenderHTGSFoveatedRecord& record);

    private:
        std::ofstream file;
};

} // namespace htgs::recording
