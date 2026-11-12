#pragma once

/**
 * Recording file format, allowing frame-perfect replay / analysis of a VR-HTGS experiment session.
 * Note that we do not include an official reader for the format in our code release.
 */

#include <cstdint>
#include <fstream>
#include <string>

#include "core/system/Matrix.hpp"
#include "core/system/Vector.hpp"

#pragma pack(push, 1)


namespace htgs::recording {

inline constexpr char FILE_MAGIC[8] = {'H', 'T', 'G', 'S', 'R', 'E', 'C', '\0'};
inline constexpr uint32_t FORMAT_VERSION = 1;

struct FileHeader {
    char magic[8];     // FILE_MAGIC
    uint32_t version;  // FORMAT_VERSION
};
static_assert(sizeof(FileHeader) == 12, "FileHeader must be 12 bytes");

enum class RecordType : uint8_t {
    SCENE_CHANGE = 0,
    FOVEA_MASK_UPDATE = 1,
    VISIBILITY_MASK_UPDATE = 2,
    FRAME_RENDER = 3,
    FRAME_BEGIN = 4
};

enum class RasterizerVariant : uint8_t {
    HTGS = 0,
    HTGS_FOVEATED = 1,
};


struct SceneChangeRecord {
    int16_t conditionIndex;
    RasterizerVariant modelType;
    // std::string modelPath;
    char modelPath[260]; // Using Windows MAX_PATH to ensure fixed size
    bool antiAliasing;
    bool blurPeriphery;
    float sceneScale;
};
static_assert(std::is_trivially_copyable_v<SceneChangeRecord>, "SceneChangeRecord must be trivially copyable");


struct FoveaMaskUpdateRecord {
    uint16_t widthTiles;
    uint16_t heightTiles;
    // Followed by ((width + 1) * (height + 1)) uint32_t values
    // -> We only store the summed area table, since the raw mask is not needed for replay
};
static_assert(std::is_trivially_copyable_v<FoveaMaskUpdateRecord>, "FoveaMaskUpdateRecord must be trivially copyable");


struct VisibilityMaskUpdateRecord {
    uint16_t widthTiles;
    uint16_t heightTiles;
    // Followed by ((widthTiles * heightTiles + 31) / 32) uint32_t values: tile i = y * widthTiles + x
    //   is bit (i % 32) of word (i / 32), 1 = visible
    // -> We only store the packed raw mask, the summed area table can be recomputed at replay time
};
static_assert(std::is_trivially_copyable_v<VisibilityMaskUpdateRecord>, "VisibilityMaskUpdateRecord must be trivially copyable");


struct FrameBeginRecord {
    int64_t displayTime;    // XrFrameState::predictedDisplayTime [ns, runtime clock; only differences are meaningful]
    int64_t displayPeriod;  // XrFrameState::predictedDisplayPeriod [ns], e.g. ~11.1 ms at 90 Hz
};
static_assert(std::is_trivially_copyable_v<FrameBeginRecord>, "FrameBeginRecord must be trivially copyable");


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
static_assert(std::is_trivially_copyable_v<FrameRenderHTGSRecord>, "FrameRenderHTGSRecord must be trivially copyable");


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

        void writeFrameBegin(const FrameBeginRecord& record);

        void writeFrameRenderHTGS(const FrameRenderHTGSRecord& record);

        void writeFrameRenderHTGSFoveated(const FrameRenderHTGSFoveatedRecord& record);

    private:
        std::ofstream file;
};

} // namespace htgs::recording

#pragma pack(pop)
