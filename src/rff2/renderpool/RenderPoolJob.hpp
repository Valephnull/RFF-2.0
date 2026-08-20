#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "../settings/Settings.h"

namespace merutilm::rff2 {

    enum class RenderPoolFrameState : uint8_t {
        WAITING,
        ASSIGNED,
        RENDERING,
        VERIFYING,
        COMPLETE,
        FAILED
    };

    struct RenderPoolFrame {
        uint32_t id = 0;
        float logZoom = 0;
        RenderPoolFrameState state = RenderPoolFrameState::WAITING;
        uint64_t workerId = 0;
        std::string workerName;
        std::string error;
        uint32_t attempts = 0;
    };

    struct RenderPoolJobManifest {
        static constexpr uint16_t FORMAT_VERSION = 1;

        uint64_t id = 0;
        uint32_t windowWidth = 0;
        uint32_t windowHeight = 0;
        float startLogZoom = 0;
        float zoomIncrement = 0;
        float clarityMultiplier = 1;
        std::string centerReal;
        std::string centerImag;

        float bailout = 2;
        bool useParallelReference = false;
        uint32_t referenceSynchronizationInterval = 0;
        uint8_t referenceSynchronizationRadiusPower = 0;
        uint32_t compressCriteria = 0;
        uint8_t compressionThresholdPower = 0;
        bool useSeriesApproximation = false;
        uint16_t appliedTermsCount = 0;
        uint16_t validatedTermsCount = 0;
        float seriesApproximationEpsilonPower = 0;
        uint16_t minimumSkippedReference = 0;
        uint8_t maximumMultiplierBetweenLevel = 0;
        float approximationEpsilonPower = 0;
        uint8_t approximationSelectionMethod = 0;
        bool compressApproximation = false;
        bool parallelizeApproximation = false;
        uint64_t maxIteration = 0;
        uint8_t decimalizeIterationMethod = 0;
        bool autoMaxIteration = false;
        uint8_t interiorDetectRadiusPower = 0;
        uint16_t autoIterationMultiplier = 0;
        bool absoluteIterationMode = false;
        uint32_t frameCount = 0;

        [[nodiscard]] std::vector<std::byte> encode() const;
        [[nodiscard]] static bool decode(std::span<const std::byte> bytes, RenderPoolJobManifest &result,
                                         std::string &error);
        [[nodiscard]] bool valid(std::string &error) const;
        void apply(Settings &settings, float logZoom, uint32_t localThreads) const;
    };

    struct RenderPoolJob {
        RenderPoolJobManifest manifest;
        std::filesystem::path outputDirectory;
        std::vector<RenderPoolFrame> frames;
        bool running = false;
        bool paused = false;

        [[nodiscard]] uint32_t completedCount() const;
    };
}
