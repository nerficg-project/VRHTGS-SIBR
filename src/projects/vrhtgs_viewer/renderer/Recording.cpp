#include "projects/vrhtgs_viewer/renderer/Recording.hpp"

#include <cstring>
#include <stdexcept>


namespace htgs::recording {

RecordingWriter::RecordingWriter(const std::string& filename) {
    file.open(filename, std::ios::binary | std::ios::out);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open recording file for writing: " + filename);
    }

    FileHeader header;
    std::memcpy(header.magic, FILE_MAGIC, sizeof(header.magic));
    header.version = FORMAT_VERSION;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
}

RecordingWriter::~RecordingWriter() {
    if (file.is_open()) {
        file.close();
    }
}

void RecordingWriter::writeSceneChange(int conditionIndex, RasterizerVariant modelType,
                                       const std::string& modelPath, float sceneScale,
                                       bool antiAliasing, bool blurPeriphery) {
    // Write record type
    RecordType recordType = RecordType::SCENE_CHANGE;
    file.write(reinterpret_cast<const char*>(&recordType), sizeof(recordType));

    // Prepare record
    SceneChangeRecord record;
    record.conditionIndex = conditionIndex;
    record.modelType = modelType;
    record.sceneScale = sceneScale;
    record.antiAliasing = antiAliasing;
    record.blurPeriphery = blurPeriphery;

    // Copy model path with bounds checking
    std::strncpy(record.modelPath, modelPath.c_str(), sizeof(record.modelPath) - 1);
    record.modelPath[sizeof(record.modelPath) - 1] = '\0';

    // Write record
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
}

void RecordingWriter::writeFoveaMaskUpdate(int widthTiles, int heightTiles,
                                           const uint32_t* summedAreaTable) {
    // Write record type
    RecordType recordType = RecordType::FOVEA_MASK_UPDATE;
    file.write(reinterpret_cast<const char*>(&recordType), sizeof(recordType));

    // Write record header
    FoveaMaskUpdateRecord record;
    record.widthTiles = widthTiles;
    record.heightTiles = heightTiles;
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));

    // Write summed area table
    size_t tableSize = (widthTiles + 1) * (heightTiles + 1);
    file.write(reinterpret_cast<const char*>(summedAreaTable),
               tableSize * sizeof(uint32_t));
}

void RecordingWriter::writeVisibilityMaskUpdate(int widthTiles, int heightTiles, const uint32_t* packedMask) {
    // Write record type
    RecordType recordType = RecordType::VISIBILITY_MASK_UPDATE;
    file.write(reinterpret_cast<const char*>(&recordType), sizeof(recordType));

    // Write record header
    VisibilityMaskUpdateRecord record;
    record.widthTiles = widthTiles;
    record.heightTiles = heightTiles;
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));

    // Write packed mask
    size_t packedSize = (widthTiles * heightTiles + sizeof(uint32_t) * 8 - 1) / (sizeof(uint32_t) * 8);
    file.write(reinterpret_cast<const char*>(packedMask),
               packedSize * sizeof(uint32_t));
}

void RecordingWriter::writeFrameBegin(const FrameBeginRecord& record) {
    // Write record type
    RecordType recordType = RecordType::FRAME_BEGIN;
    file.write(reinterpret_cast<const char*>(&recordType), sizeof(recordType));

    // Write record
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
}

void RecordingWriter::writeFrameRenderHTGS(const FrameRenderHTGSRecord& record) {
    // Write record type
    RecordType recordType = RecordType::FRAME_RENDER;
    file.write(reinterpret_cast<const char*>(&recordType), sizeof(recordType));

    // Write variant identifier (HTGS = 0)
    RasterizerVariant variant = static_cast<RasterizerVariant>(0);
    file.write(reinterpret_cast<const char*>(&variant), sizeof(variant));

    // Write record
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
}

void RecordingWriter::writeFrameRenderHTGSFoveated(const FrameRenderHTGSFoveatedRecord& record) {
    // Write record type
    RecordType recordType = RecordType::FRAME_RENDER;
    file.write(reinterpret_cast<const char*>(&recordType), sizeof(recordType));

    // Write variant identifier (HTGS_FOVEATED = 1)
    RasterizerVariant variant = static_cast<RasterizerVariant>(1);
    file.write(reinterpret_cast<const char*>(&variant), sizeof(variant));

    // Write record
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
}

} // namespace htgs::recording
