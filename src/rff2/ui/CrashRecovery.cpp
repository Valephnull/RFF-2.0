#include "CrashRecovery.hpp"

#include <system_error>

#include "RFF2.hpp"
#include "imgui.h"
#include "vulkan_helper/base/logger.hpp"
#include "vulkan_helper/util/ExecutableUtils.hpp"

namespace merutilm::rff2 {
    namespace {
        constexpr double AUTOSAVE_INTERVAL_SECONDS = 1.0;

        [[nodiscard]] bool newerThan(const std::filesystem::path &left, const std::filesystem::path &right) {
            std::error_code leftError;
            std::error_code rightError;
            const auto leftTime = std::filesystem::last_write_time(left, leftError);
            const auto rightTime = std::filesystem::last_write_time(right, rightError);
            return !leftError && (rightError || leftTime >= rightTime);
        }
    } // namespace

    void CrashRecovery::initialize() {
        if (initialized)
            return;
        initialized = true;

        const std::filesystem::path directory = vkh::ExecutableUtils::getExecutableDirectory();
        recoveryPath = directory / "RFF-EXP.recovery.rfl";
        temporaryPath = directory / "RFF-EXP.recovery.tmp.rfl";

        const std::optional<RFFLocationBinary> primary = readValid(recoveryPath);
        const std::optional<RFFLocationBinary> temporary = readValid(temporaryPath);
        if (primary && temporary) {
            pendingRecovery = newerThan(temporaryPath, recoveryPath) ? temporary : primary;
        } else if (primary) {
            pendingRecovery = primary;
        } else if (temporary) {
            pendingRecovery = temporary;
        }

        if (pendingRecovery) {
            recoveryKind = RecoveryKind::CRASH_AUTOSAVE;
        } else if (const std::optional<RFFLocationBinary> lastRendered =
                           readValid(RFF2::getBackupLocationPath())) {
            pendingRecovery = lastRendered;
            recoveryKind = RecoveryKind::LAST_RENDERED;
        }

        awaitingDecision = pendingRecovery.has_value();
        if (!awaitingDecision) {
            recoveryKind = RecoveryKind::NONE;
            discardRecovery();
        }
    }

    void CrashRecovery::update(RFF2 &app) {
        if (!initialized || awaitingDecision || app.isNavigationLocked())
            return;

        const double now = app.getWindowContext().getWindow()->getTime();
        if (lastSaveTime < 0 || now - lastSaveTime >= AUTOSAVE_INTERVAL_SECONDS) {
            lastSaveTime = now;
            if (locationChanged(app))
                saveNow(app);
        }
    }

    void CrashRecovery::renderImGui(RFF2 &app) {
        if (!awaitingDecision)
            return;

        const bool crashAutosave = recoveryKind == RecoveryKind::CRASH_AUTOSAVE;
        const char *title = crashAutosave ? "Recover Previous Session" : "Reload Last Rendered Location";

        if (!popupRequested) {
            ImGui::OpenPopup(title);
            popupRequested = true;
        }

        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        if (crashAutosave) {
            ImGui::TextWrapped(
                    "RFF-EXP did not finish its previous session cleanly. Recover the last autosaved location?");
        } else {
            ImGui::TextWrapped("RFF-EXP found the location from the last completed render. Reload it?");
        }
        if (pendingRecovery) {
            ImGui::Separator();
            ImGui::Text("Log zoom: %.6f", pendingRecovery->getLogZoom());
            ImGui::Text("Iterations: %llu",
                        static_cast<unsigned long long>(pendingRecovery->getMaxIteration()));
        }

        const char *acceptLabel = crashAutosave ? "Recover" : "Reload";
        if (ImGui::Button(acceptLabel, ImVec2(180, 0))) {
            if (pendingRecovery) {
                Settings &settings = app.getSettings();
                settings.fractal.reference.center = fixed_point_complex_i1(
                        pendingRecovery->getReal(), pendingRecovery->getImag(),
                        Perturbator::logZoomToExp10(pendingRecovery->getLogZoom()));
                settings.fractal.general.logZoom = pendingRecovery->getLogZoom();
                settings.fractal.perturb.maxIteration = pendingRecovery->getMaxIteration();
                settings.fractal.reference.reuse = false;
                app.getRequests().requestRecompute();
            }
            awaitingDecision = false;
            pendingRecovery.reset();
            recoveryKind = RecoveryKind::NONE;
            lastSaveTime = -1;
            lastReal.clear();
            lastImag.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const char *declineLabel = crashAutosave ? "Start Fresh" : "Keep Current";
        if (ImGui::Button(declineLabel, ImVec2(180, 0))) {
            if (crashAutosave)
                discardRecovery();
            awaitingDecision = false;
            pendingRecovery.reset();
            recoveryKind = RecoveryKind::NONE;
            lastSaveTime = -1;
            lastReal.clear();
            lastImag.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void CrashRecovery::cleanShutdown() { discardRecovery(); }

    void CrashRecovery::saveNow(RFF2 &app) {
        if (!initialized || awaitingDecision)
            return;

        FractalSettings &fractal = app.getSettings().fractal;
        const std::string real = fractal.reference.center.real.to_string();
        const std::string imag = fractal.reference.center.imag.to_string();
        RFFLocationBinary(fractal.general.logZoom, real, imag, fractal.perturb.maxIteration)
                .exportFile(temporaryPath);

        std::error_code error;
        if (std::filesystem::exists(temporaryPath, error)) {
            error.clear();
            std::filesystem::copy_file(temporaryPath, recoveryPath,
                                       std::filesystem::copy_options::overwrite_existing, error);
        }
        if (error) {
            vkh::logger::log_err("Could not update crash recovery autosave: {}", error.message());
            return;
        }
        rememberLocation(app);
    }

    std::optional<RFFLocationBinary> CrashRecovery::readValid(const std::filesystem::path &path) {
        const RFFLocationBinary location = RFFLocationBinary::read(path);
        if (!location.hasData())
            return std::nullopt;
        return location;
    }

    bool CrashRecovery::locationChanged(RFF2 &app) const {
        FractalSettings &fractal = app.getSettings().fractal;
        return lastReal != fractal.reference.center.real.to_string() ||
               lastImag != fractal.reference.center.imag.to_string() ||
               lastLogZoom != fractal.general.logZoom || lastMaxIteration != fractal.perturb.maxIteration;
    }

    void CrashRecovery::rememberLocation(RFF2 &app) {
        FractalSettings &fractal = app.getSettings().fractal;
        lastReal = fractal.reference.center.real.to_string();
        lastImag = fractal.reference.center.imag.to_string();
        lastLogZoom = fractal.general.logZoom;
        lastMaxIteration = fractal.perturb.maxIteration;
    }

    void CrashRecovery::discardRecovery() {
        std::error_code error;
        std::filesystem::remove(recoveryPath, error);
        error.clear();
        std::filesystem::remove(temporaryPath, error);
    }
} // namespace merutilm::rff2
