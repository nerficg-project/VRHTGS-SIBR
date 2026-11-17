#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "yaml-cpp/yaml.h"

#include "core/assets/InputCamera.hpp"
#include "core/system/Config.hpp"


namespace sibr {
    /// Defaults used when the model config or the command line don't say otherwise
    constexpr float DEFAULT_NEAR_PLANE = 0.2f;
    constexpr float DEFAULT_FAR_PLANE = 1000.0f;
    constexpr float DEFAULT_DESKTOP_FOVY_DEGREES = 60.0f;  // Desktop view only; OpenXR uses the headset's per-eye FOV
    constexpr int DEFAULT_DESKTOP_WIDTH = 1280;
    constexpr int DEFAULT_DESKTOP_HEIGHT = 720;

    /**
     * Desktop rendering resolution: --rendering-size if both components are set, the defaults otherwise.
     */
    inline Vector2u desktopResolution(const Vector2i& renderingSize) {
        if (renderingSize.x() > 0 && renderingSize.y() > 0) {
            return renderingSize.cast<uint32_t>();
        }
        return Vector2u(DEFAULT_DESKTOP_WIDTH, DEFAULT_DESKTOP_HEIGHT);
    }

    /**
     * Start camera with default desktop intrinsics, at the origin with the identity rotation (OpenGL axes).
     * znear/zfar also reach the headset: OpenXRRdrMode copies them into both eye cameras.
     */
    inline InputCamera makeStartCamera(const Vector2u& resolution, float znear, float zfar) {
        Camera cam;
        cam.fovy(DEFAULT_DESKTOP_FOVY_DEGREES * float(M_PI) / 180.0f);
        cam.znear(znear);
        cam.zfar(zfar);
        cam.position(Vector3f::Zero());
        return InputCamera(cam, int(resolution.x()), int(resolution.y()));
    }

    /**
     * The parts of a model's nerficg training_config.yaml the apps use. Every field is optional: models without
     * a config (or with keys missing) fall back to the apps' defaults. The dataset itself is never loaded.
     */
    struct ModelConfig {
        bool found = false;               ///< Whether training_config.yaml exists and could be parsed
        std::optional<float> nearPlane;   ///< DATASET.NEAR_PLANE
        std::optional<float> farPlane;    ///< DATASET.FAR_PLANE
        std::filesystem::path datasetPath;  ///< DATASET.PATH as written (may be relative to the nerficg root)

        /**
         * Name identifying the dataset, used to share calibrations between models trained on the same data:
         * the last two components of DATASET.PATH (e.g. "mipnerf360/bicycle"), empty if the path is unknown.
         */
        std::filesystem::path datasetName() const {
            std::filesystem::path p = datasetPath;
            if (!p.empty() && !p.has_filename()) {
                p = p.parent_path();  // Trailing slash
            }
            if (p.empty()) {
                return {};
            }
            const std::filesystem::path parent = p.parent_path().filename();
            return parent.empty() ? p.filename() : parent / p.filename();
        }
    };

    namespace detail {
        template<typename T>
        std::optional<T> readYamlValue(const YAML::Node& root, const char* section, const char* key) {
            try {
                const YAML::Node node = root[section][key];
                if (node && node.IsScalar()) {
                    return node.as<T>();
                }
            } catch (const YAML::Exception&) {
                // Missing or malformed values silently fall back to the defaults
            }
            return std::nullopt;
        }
    }

    /**
     * Read training_config.yaml from a model directory. Returns an empty config if the file is missing or invalid.
     */
    inline ModelConfig loadModelConfig(const std::filesystem::path& modelDir) {
        ModelConfig config;
        const std::filesystem::path configPath = modelDir / "training_config.yaml";
        if (!std::filesystem::exists(configPath)) {
            SIBR_LOG << "No training_config.yaml in " << modelDir << ", using default settings." << std::endl;
            return config;
        }

        YAML::Node root;
        try {
            root = YAML::LoadFile(configPath.string());
        } catch (const YAML::Exception& e) {
            SIBR_WRG << "Could not parse " << configPath << " (" << e.what() << "), using default settings." << std::endl;
            return config;
        }
        config.found = true;

        config.nearPlane = detail::readYamlValue<float>(root, "DATASET", "NEAR_PLANE");
        config.farPlane = detail::readYamlValue<float>(root, "DATASET", "FAR_PLANE");
        if (auto path = detail::readYamlValue<std::string>(root, "DATASET", "PATH")) {
            config.datasetPath = *path;
        }
        const bool invalidNear = config.nearPlane && !(*config.nearPlane > 0.0f);
        const bool invalidFar = config.farPlane && !(*config.farPlane > 0.0f);
        const bool swapped = config.nearPlane && config.farPlane && !(*config.nearPlane < *config.farPlane);
        if (invalidNear || invalidFar || swapped) {
            SIBR_WRG << "Ignoring invalid DATASET.NEAR_PLANE / FAR_PLANE in " << configPath << "." << std::endl;
            config.nearPlane.reset();
            config.farPlane.reset();
        }
        return config;
    }

    /**
     * Whether a file looks like a PLY the Gaussian loader (loadPly in GaussianView.cpp) accepts: binary little-endian,
     * a non-empty vertex element with only float properties, all required 3DGS properties present, and enough data
     * after the header. Only the header is parsed; the vertex data itself isn't read.
     */
    inline bool isLoadableGaussianPly(const std::filesystem::path& file) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return false;
        }
        std::ifstream in(file, std::ios_base::binary);
        std::string line;
        if (!std::getline(in, line) || line.rfind("ply", 0) != 0) {
            return false;
        }

        bool binaryLE = false;
        bool inVertexElement = false;
        bool endHeader = false;
        long long count = 0;
        std::vector<std::string> properties;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::stringstream ss(line);
            std::string keyword;
            ss >> keyword;
            if (keyword == "end_header") {
                endHeader = true;
                break;
            } else if (keyword == "format") {
                std::string format;
                ss >> format;
                binaryLE = (format == "binary_little_endian");
            } else if (keyword == "element") {
                std::string name;
                ss >> name;
                inVertexElement = (name == "vertex");
                if (inVertexElement && !(ss >> count)) {
                    return false;
                }
            } else if (keyword == "property" && inVertexElement) {
                std::string type, name;
                ss >> type >> name;
                if (type != "float" && type != "float32") {
                    return false;
                }
                properties.push_back(name);
            }
        }
        if (!endHeader || !binaryLE || count <= 0) {
            return false;
        }

        for (const char* required : { "x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                                      "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3" }) {
            if (std::find(properties.begin(), properties.end(), required) == properties.end()) {
                return false;
            }
        }

        const std::streamoff headerSize = in.tellg();
        const auto fileSize = std::filesystem::file_size(file, ec);
        if (ec || headerSize < 0) {
            return false;
        }
        const unsigned long long dataSize = static_cast<unsigned long long>(count) * properties.size() * sizeof(float);
        return fileSize >= static_cast<unsigned long long>(headerSize) + dataSize;
    }

    /**
     * The Gaussian PLY of a model directory: gaussians.ply, else final.ply (nerficg's vrhtgs_export_model.py),
     * else the first loadable .ply in the directory in alphanumerical order. Empty if there is none.
     */
    inline std::filesystem::path findModelPly(const std::filesystem::path& modelDir) {
        for (const char* name : { "gaussians.ply", "final.ply" }) {
            const std::filesystem::path candidate = modelDir / name;
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }

        std::vector<std::filesystem::path> plyFiles;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(modelDir, ec)) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".ply") {
                plyFiles.push_back(entry.path());
            }
        }
        std::sort(plyFiles.begin(), plyFiles.end(), [](const auto& a, const auto& b) {
            return a.filename().native() < b.filename().native();
        });
        for (const auto& file : plyFiles) {
            if (isLoadableGaussianPly(file)) {
                SIBR_LOG << "No 'gaussians.ply' or 'final.ply' in " << modelDir << ", using " << file.filename() << std::endl;
                return file;
            }
            SIBR_WRG << "Skipping " << file << ": not a binary 3DGS PLY the loader can read." << std::endl;
        }
        return {};
    }
}
