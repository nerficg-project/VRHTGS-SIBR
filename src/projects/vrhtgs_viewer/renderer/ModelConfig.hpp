#pragma once

#include <filesystem>
#include <optional>
#include <string>

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
}
