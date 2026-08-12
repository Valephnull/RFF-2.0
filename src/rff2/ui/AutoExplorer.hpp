#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace merutilm::rff2 {
    class RFF2;

    class AutoExplorer final {
    public:
        struct Config {
            float zoomIncrement = 1.0f;
            float stopLogZoom = 100.0f;
            float minimumContrast = 12.0f;
            int candidateSamples = 4096;
            int edgeMarginPercent = 8;
            uint32_t seed = 0;
            bool useBestFallback = true;
            bool recoverWhenStuck = true;
            float recoveryZoomOut = 0.5f;
            int recoveryAvoidRadiusPercent = 10;
        } config;

        void start(RFF2 &app);
        void stop();
        void update(RFF2 &app);

        [[nodiscard]] bool isRunning() const { return running; }
        [[nodiscard]] uint64_t getStepCount() const { return stepCount; }
        [[nodiscard]] uint64_t getRecoveryCount() const { return recoveryCount; }
        [[nodiscard]] const std::string &getStatus() const { return status; }

    private:
        struct Candidate {
            uint16_t x = 0;
            uint16_t y = 0;
            double contrast = 0;
            bool meetsThreshold = false;
        };

        bool running = false;
        bool waitingForRender = false;
        bool recoveryInProgress = false;
        bool avoidCandidate = false;
        float avoidXRatio = 0;
        float avoidYRatio = 0;
        uint64_t lastCompletedRender = 0;
        uint64_t stepCount = 0;
        uint64_t recoveryCount = 0;
        uint64_t consecutiveRecoveryCount = 0;
        std::mt19937 random = {};
        std::string status = "Stopped";

        [[nodiscard]] Candidate findCandidate(const RFF2 &app);
        bool advance(RFF2 &app);
        bool recover(RFF2 &app, const Candidate &failedCandidate, std::string_view reason);
    };
} // namespace merutilm::rff2
