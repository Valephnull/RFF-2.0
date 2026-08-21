#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "../io/RFFLocationBinary.h"

namespace merutilm::rff2 {
    class RFF2;

    class CrashRecovery final {
        enum class RecoveryKind : uint8_t { NONE, CRASH_AUTOSAVE, LAST_RENDERED };

        std::filesystem::path recoveryPath;
        std::filesystem::path temporaryPath;
        std::optional<RFFLocationBinary> pendingRecovery;
        RecoveryKind recoveryKind = RecoveryKind::NONE;
        bool initialized = false;
        bool awaitingDecision = false;
        bool popupRequested = false;
        double lastSaveTime = -1;
        float lastLogZoom = 0;
        uint64_t lastMaxIteration = 0;
        std::string lastReal;
        std::string lastImag;

    public:
        void initialize();
        void update(RFF2 &app);
        void renderImGui(RFF2 &app);
        void cleanShutdown();

    private:
        [[nodiscard]] static std::optional<RFFLocationBinary> readValid(const std::filesystem::path &path);
        [[nodiscard]] bool locationChanged(RFF2 &app) const;
        void saveNow(RFF2 &app);
        void rememberLocation(RFF2 &app);
        void discardRecovery();
    };
} // namespace merutilm::rff2
