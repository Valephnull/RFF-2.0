#include "RenderPool.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>

#include "../constants/Constants.hpp"
#include "../io/RFFDynamicMapBinary.h"
#include "../io/RFFLocationBinary.h"
#include "../ui/IOUtilities.h"
#include "../ui/RFF2.hpp"
#include "../util/RendererUtils.hpp"
#include "RenderPoolBinary.hpp"
#include "RenderPoolJob.hpp"
#include "RenderPoolNetwork.hpp"
#include "RenderPoolUpnp.hpp"
#include "imgui.h"
#include "vulkan_helper/base/logger.hpp"

namespace merutilm::rff2 {
    namespace {
        enum class PoolRole : uint8_t { NONE, HOST, WORKER };
        enum class PoolConnectionMode : uint8_t { INTERNET, LAN };
        enum class LocalRenderPhase : uint8_t { IDLE, RENDERING };

        struct PoolTask {
            uint64_t jobId = 0;
            uint32_t frameId = 0;
            float logZoom = 0;
            uint64_t workerId = 0;
            std::string workerName;
        };

        struct WorkerInfo {
            uint64_t id = 0;
            std::string name;
        };

        constexpr std::string_view JOB_MANIFEST_FILENAME = ".rff-render-pool-job";
        constexpr double TASK_REQUEST_INTERVAL_SECONDS = 1.0;

        uint8_t poolIdChecksum(const uint32_t address) {
            uint32_t value = address;
            uint32_t checksum = 7;
            while (value > 0) {
                checksum = (checksum * 3 + value % 10) % 10;
                value /= 10;
            }
            return static_cast<uint8_t>(checksum);
        }

        bool parseIPv4(const std::string &text, uint32_t &result) {
            std::array<uint32_t, 4> parts{};
            size_t start = 0;
            for (size_t i = 0; i < parts.size(); ++i) {
                const size_t end = i + 1 == parts.size() ? text.size() : text.find('.', start);
                if (end == std::string::npos || end == start)
                    return false;
                const auto conversion = std::from_chars(text.data() + start, text.data() + end, parts[i]);
                if (conversion.ec != std::errc{} || conversion.ptr != text.data() + end || parts[i] > 255)
                    return false;
                start = end + 1;
            }
            result = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
            return true;
        }

        std::string formatIPv4(const uint32_t address) {
            return std::format("{}.{}.{}.{}", address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff,
                               address & 0xff);
        }

        std::string encodePoolId(const std::string &address) {
            uint32_t numericAddress = 0;
            if (!parseIPv4(address, numericAddress))
                return {};
            return std::to_string(static_cast<uint64_t>(numericAddress) * 10 + poolIdChecksum(numericAddress));
        }

        bool decodePoolId(const std::string_view text, std::string &address) {
            uint64_t encoded = 0;
            const auto conversion = std::from_chars(text.data(), text.data() + text.size(), encoded);
            if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size())
                return false;
            const uint64_t addressValue = encoded / 10;
            if (addressValue > UINT32_MAX || encoded % 10 != poolIdChecksum(static_cast<uint32_t>(addressValue)))
                return false;
            address = formatIPv4(static_cast<uint32_t>(addressValue));
            return true;
        }

        std::string defaultWorkerName() {
#ifdef _WIN32
            if (const char *name = std::getenv("COMPUTERNAME"); name != nullptr && *name != '\0')
                return name;
#else
            if (const char *name = std::getenv("HOSTNAME"); name != nullptr && *name != '\0')
                return name;
#endif
            return "RFF Worker";
        }

        uint64_t newJobId() {
            const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            std::random_device random;
            return now ^ (static_cast<uint64_t>(random()) << 32) ^ random();
        }

        std::vector<std::byte> encodeTask(const PoolTask &task) {
            RenderPoolBinaryWriter writer;
            writer.integer(task.jobId);
            writer.integer(task.frameId);
            writer.floating(task.logZoom);
            return writer.take();
        }

        bool decodeTask(const std::span<const std::byte> bytes, PoolTask &task) {
            RenderPoolBinaryReader reader(bytes);
            return reader.integer(task.jobId) && reader.integer(task.frameId) && reader.floating(task.logZoom) &&
                   reader.finished();
        }

        std::vector<std::byte> encodeWorkerState(const PoolTask &task, const RenderPoolFrameState state) {
            RenderPoolBinaryWriter writer;
            writer.integer(task.jobId);
            writer.integer(task.frameId);
            writer.integer(static_cast<uint8_t>(state));
            return writer.take();
        }

        bool decodeWorkerState(const std::span<const std::byte> bytes, uint64_t &jobId, uint32_t &frameId,
                               RenderPoolFrameState &state) {
            RenderPoolBinaryReader reader(bytes);
            uint8_t rawState = 0;
            if (!reader.integer(jobId) || !reader.integer(frameId) || !reader.integer(rawState) ||
                rawState > static_cast<uint8_t>(RenderPoolFrameState::FAILED) || !reader.finished()) {
                return false;
            }
            state = static_cast<RenderPoolFrameState>(rawState);
            return frameId > 0;
        }

        std::vector<std::byte> encodeJobState(const RenderPoolJob &job) {
            RenderPoolBinaryWriter writer;
            writer.integer(job.manifest.id);
            writer.integer(job.completedCount());
            writer.integer(job.manifest.frameCount);
            writer.boolean(job.running);
            writer.boolean(job.paused);
            return writer.take();
        }

        std::vector<std::byte> encodeFrameStates(const uint64_t jobId, const std::span<const RenderPoolFrame> frames) {
            RenderPoolBinaryWriter writer;
            writer.integer(jobId);
            writer.integer(static_cast<uint32_t>(frames.size()));
            for (const auto &frame: frames) {
                writer.integer(frame.id);
                writer.integer(static_cast<uint8_t>(frame.state));
            }
            return writer.take();
        }

        bool decodeFrameStates(const std::span<const std::byte> bytes, uint64_t &jobId,
                               std::vector<std::pair<uint32_t, RenderPoolFrameState>> &updates) {
            RenderPoolBinaryReader reader(bytes);
            uint32_t count = 0;
            if (!reader.integer(jobId) || !reader.integer(count) || count > 1000000 ||
                reader.remaining() != static_cast<size_t>(count) * (sizeof(uint32_t) + sizeof(uint8_t))) {
                return false;
            }
            updates.clear();
            updates.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                uint32_t frameId = 0;
                uint8_t rawState = 0;
                if (!reader.integer(frameId) || !reader.integer(rawState) || frameId == 0 ||
                    rawState > static_cast<uint8_t>(RenderPoolFrameState::FAILED)) {
                    return false;
                }
                updates.emplace_back(frameId, static_cast<RenderPoolFrameState>(rawState));
            }
            return reader.finished();
        }

        std::vector<RenderPoolFrame> createWorkerFrames(const RenderPoolJobManifest &manifest) {
            std::vector<RenderPoolFrame> frames;
            frames.reserve(manifest.frameCount);
            float logZoom = manifest.startLogZoom;
            for (uint32_t id = 1; id <= manifest.frameCount; ++id, logZoom -= manifest.zoomIncrement)
                frames.push_back(RenderPoolFrame{.id = id, .logZoom = logZoom});
            return frames;
        }

        bool writeManifest(const RenderPoolJob &job, std::string &error) {
            const auto path = job.outputDirectory / JOB_MANIFEST_FILENAME;
            const auto temporary = path.string() + ".partial";
            const auto bytes = job.manifest.encode();
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) {
                    error = "Could not write the render-pool job manifest";
                    return false;
                }
                output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!output) {
                    error = "Could not finish the render-pool job manifest";
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                error = "Could not install the render-pool job manifest: " + ec.message();
                return false;
            }
            return true;
        }

        bool readManifest(const std::filesystem::path &directory, RenderPoolJobManifest &manifest, std::string &error) {
            const auto path = directory / JOB_MANIFEST_FILENAME;
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                error = "That folder does not contain a render-pool job";
                return false;
            }
            const auto length = input.tellg();
            if (length <= 0 || length > static_cast<std::streamoff>(1U << 20)) {
                error = "The render-pool job manifest has an invalid size";
                return false;
            }
            input.seekg(0, std::ios::beg);
            std::vector<std::byte> bytes(static_cast<size_t>(length));
            input.read(reinterpret_cast<char *>(bytes.data()), length);
            return input && RenderPoolJobManifest::decode(bytes, manifest, error);
        }

        std::vector<RenderPoolFrame> createFrames(const RenderPoolJobManifest &manifest,
                                                  const std::filesystem::path &directory) {
            std::vector<RenderPoolFrame> frames;
            frames.reserve(manifest.frameCount);
            float logZoom = manifest.startLogZoom;
            for (uint32_t id = 1; id <= manifest.frameCount; ++id, logZoom -= manifest.zoomIncrement) {
                RenderPoolFrame frame{.id = id, .logZoom = logZoom};
                const auto map = RFFDynamicMapBinary::readByID(directory, id);
                const auto location = RFFLocationBinary::read(
                        directory / IOUtilities::fileNameFormat(id, Constants::File::EXT_LOCATION));
                if (map.hasData() && location.hasData() && std::abs(map.getLogZoom() - logZoom) < 0.0005f &&
                    std::abs(location.getLogZoom() - logZoom) < 0.0005f) {
                    frame.state = RenderPoolFrameState::COMPLETE;
                }
                frames.push_back(std::move(frame));
            }
            return frames;
        }

        ImVec4 frameColor(const RenderPoolFrameState state) {
            switch (state) {
                case RenderPoolFrameState::WAITING:
                    return {0.30f, 0.30f, 0.32f, 1};
                case RenderPoolFrameState::ASSIGNED:
                    return {0.23f, 0.46f, 0.76f, 1};
                case RenderPoolFrameState::RENDERING:
                    return {0.18f, 0.56f, 0.88f, 1};
                case RenderPoolFrameState::VERIFYING:
                    return {0.84f, 0.65f, 0.18f, 1};
                case RenderPoolFrameState::COMPLETE:
                    return {0.24f, 0.68f, 0.36f, 1};
                case RenderPoolFrameState::FAILED:
                    return {0.78f, 0.24f, 0.24f, 1};
            }
            return {0.3f, 0.3f, 0.3f, 1};
        }

        const char *frameStateText(const RenderPoolFrameState state) {
            switch (state) {
                case RenderPoolFrameState::WAITING:
                    return "Waiting";
                case RenderPoolFrameState::ASSIGNED:
                    return "Assigned";
                case RenderPoolFrameState::RENDERING:
                    return "Rendering";
                case RenderPoolFrameState::VERIFYING:
                    return "Verifying";
                case RenderPoolFrameState::COMPLETE:
                    return "Complete";
                case RenderPoolFrameState::FAILED:
                    return "Failed";
            }
            return "Unknown";
        }
    } // namespace

    struct RenderPool::Impl {
        RenderPoolNetwork network;
        PoolRole role = PoolRole::NONE;
        PoolConnectionMode connectionMode = PoolConnectionMode::INTERNET;
        bool waitingForPassword = false;
        bool authenticated = false;
        bool workerRenders = true;
        bool hostRenders = true;
        std::array<char, 32> poolIdInput{};
        std::array<char, 128> passwordInput{};
        std::array<char, 128> workerNameInput{};
        std::string poolId;
        std::string upnpStatus;
        std::optional<std::future<RenderPoolUpnpResult>> upnpFuture;
        std::optional<RenderPoolUpnpMapping> upnpMapping;
        std::string status = "Not connected";
        std::unordered_map<uint64_t, WorkerInfo> workers;
        std::optional<RenderPoolJob> hostJob;
        std::optional<RenderPoolJobManifest> workerJob;
        std::vector<RenderPoolFrame> workerFrames;
        bool workerJobRunning = false;
        bool workerJobPaused = false;
        uint32_t workerCompletedFrames = 0;
        std::optional<PoolTask> queuedTask;
        std::optional<PoolTask> localTask;
        LocalRenderPhase localPhase = LocalRenderPhase::IDLE;
        uint64_t expectedRenderCount = 0;
        std::optional<Settings> savedSettings;
        VkExtent2D savedWindowExtent{};
        uint32_t localThreads = 1;
        double lastTaskRequest = -10;

        Impl() {
            const std::string workerName = defaultWorkerName();
            std::copy_n(workerName.c_str(), std::min(workerName.size(), workerNameInput.size() - 1),
                        workerNameInput.begin());
        }

        void clearConnectionState() {
            role = PoolRole::NONE;
            waitingForPassword = false;
            authenticated = false;
            workers.clear();
            workerJob.reset();
            workerFrames.clear();
            workerJobRunning = false;
            workerJobPaused = false;
            workerCompletedFrames = 0;
            queuedTask.reset();
            poolId.clear();
        }

        void pollUpnp() {
            if (!upnpFuture || upnpFuture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            RenderPoolUpnpResult result;
            try {
                result = upnpFuture->get();
            } catch (const std::exception &exception) {
                upnpFuture.reset();
                upnpStatus = "UPnP setup failed";
                status = std::string("UPnP setup failed: ") + exception.what();
                return;
            }
            upnpFuture.reset();
            if (role != PoolRole::HOST || connectionMode != PoolConnectionMode::INTERNET) {
                if (result.mapping.mapped)
                    RenderPoolUpnp::close(result.mapping);
                return;
            }
            upnpStatus = result.message;
            if (result.mapping.mapped)
                upnpMapping = result.mapping;
            if (result.succeeded()) {
                poolId = encodePoolId(result.mapping.externalAddress);
                if (poolId.empty())
                    upnpStatus = "UPnP returned an invalid public address";
                else
                    status = "Internet render pool is ready; share the ID and password";
            } else {
                status = result.message;
            }
        }

        void stopUpnp() {
            if (upnpFuture) {
                RenderPoolUpnpResult result;
                try {
                    result = upnpFuture->get();
                } catch (const std::exception &) {
                    upnpFuture.reset();
                }
                if (upnpFuture) {
                    upnpFuture.reset();
                    if (result.mapping.mapped)
                        RenderPoolUpnp::close(result.mapping);
                }
            }
            if (upnpMapping) {
                RenderPoolUpnp::close(*upnpMapping);
                upnpMapping.reset();
            }
            upnpStatus.clear();
        }

        void startHost() {
            role = PoolRole::HOST;
            authenticated = true;
            poolId.clear();
            if (connectionMode == PoolConnectionMode::LAN) {
                network.startHost("", true);
                status = "Starting automatic LAN render pool";
                return;
            }
            if (passwordInput[0] == '\0') {
                role = PoolRole::NONE;
                authenticated = false;
                status = "Enter a password before starting an Internet pool";
                return;
            }
            network.startHost(passwordInput.data(), false);
            upnpStatus = "Finding the router and opening the render-pool port...";
            try {
                upnpFuture.emplace(std::async(std::launch::async, [] {
                    return RenderPoolUpnp::open(RenderPoolNetwork::PORT);
                }));
            } catch (const std::exception &exception) {
                upnpStatus = "UPnP setup could not start";
                status = std::string("UPnP setup could not start: ") + exception.what();
                return;
            }
            status = "Starting password-protected Internet render pool";
        }

        void joinPool() {
            if (connectionMode == PoolConnectionMode::LAN) {
                network.joinLan(workerNameInput.data());
                role = PoolRole::WORKER;
                waitingForPassword = false;
                authenticated = false;
                status = "Searching for a render pool on this LAN";
                return;
            }
            if (passwordInput[0] == '\0') {
                status = "Enter the render-pool password";
                return;
            }
            std::string address;
            if (!decodePoolId(poolIdInput.data(), address)) {
                status = "That render-pool ID is invalid";
                return;
            }
            network.join(address, workerNameInput.data());
            role = PoolRole::WORKER;
            waitingForPassword = false;
            authenticated = false;
            status = "Connecting to " + address;
        }

        void restoreLocalState(RFF2 &app) {
            if (!savedSettings)
                return;
            app.getState().cancel();
            app.getSettings() = *savedSettings;
            app.getWindowContext().getWindow()->setResolution(static_cast<int>(savedWindowExtent.width),
                                                              static_cast<int>(savedWindowExtent.height));
            app.getRequests().requestResize(savedWindowExtent);
            app.unlockNavigationWhenRenderFinishes();
            savedSettings.reset();
            localTask.reset();
            queuedTask.reset();
            localPhase = LocalRenderPhase::IDLE;
        }

        void leave(RFF2 &app) {
            const bool restoringView = savedSettings.has_value();
            restoreLocalState(app);
            network.stop();
            stopUpnp();
            clearConnectionState();
            status = "Not connected";
            if (!restoringView)
                app.unlockNavigationNow();
        }

        bool createJobFromCurrent(RFF2 &app, const std::filesystem::path &directory) {
            if (app.getSettings().video.data.isStatic) {
                status = "Render Pool currently supports dynamic .rfm keyframes only";
                return false;
            }
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                status = "Could not create the keyframe folder: " + ec.message();
                return false;
            }
            auto &settings = app.getSettings();
            auto &fractal = settings.fractal;
            const auto extent = app.getWindowContext().getSwapchain().getSwapchainExtent();
            RenderPoolJob job;
            auto &manifest = job.manifest;
            manifest.id = newJobId();
            manifest.windowWidth = extent.width;
            manifest.windowHeight = extent.height;
            manifest.startLogZoom = fractal.general.logZoom;
            manifest.zoomIncrement = std::log10(settings.video.data.defaultZoomIncrement);
            manifest.clarityMultiplier = settings.render.clarityMultiplier;
            manifest.centerReal = fractal.reference.center.real.to_string();
            manifest.centerImag = fractal.reference.center.imag.to_string();
            manifest.bailout = fractal.general.bailout;
            manifest.useParallelReference = fractal.reference.useParallelRefCalculation;
            manifest.referenceSynchronizationInterval = fractal.reference.sync.referenceSynchronizationInterval;
            manifest.referenceSynchronizationRadiusPower = fractal.reference.sync.referenceSynchronizationRadiusPower;
            manifest.compressCriteria = fractal.reference.compression.compressCriteria;
            manifest.compressionThresholdPower = fractal.reference.compression.compressionThresholdPower;
            manifest.useSeriesApproximation = fractal.sa.use;
            manifest.appliedTermsCount = fractal.sa.appliedTermsCount;
            manifest.validatedTermsCount = fractal.sa.validatedTermsCount;
            manifest.seriesApproximationEpsilonPower = fractal.sa.epsilonPower;
            manifest.minimumSkippedReference = fractal.mpa.minSkipReference;
            manifest.maximumMultiplierBetweenLevel = fractal.mpa.maxMultiplierBetweenLevel;
            manifest.approximationEpsilonPower = fractal.mpa.epsilonPower;
            manifest.approximationSelectionMethod = static_cast<uint8_t>(fractal.mpa.mpaSelectionMethod);
            manifest.compressApproximation = fractal.mpa.useCompress;
            manifest.parallelizeApproximation = fractal.mpa.useParallelization;
            manifest.maxIteration = fractal.perturb.maxIteration;
            manifest.decimalizeIterationMethod = static_cast<uint8_t>(fractal.perturb.decimalizeIterationMethod);
            manifest.autoMaxIteration = fractal.perturb.autoMaxIteration;
            manifest.interiorDetectRadiusPower = fractal.perturb.interiorDetectRadiusPower;
            manifest.autoIterationMultiplier = fractal.perturb.autoIterationMultiplier;
            manifest.absoluteIterationMode = fractal.perturb.absoluteIterationMode;
            if (!std::isfinite(manifest.zoomIncrement) || manifest.zoomIncrement <= 0) {
                status = "The video zoom increment is invalid";
                return false;
            }
            for (float logZoom = manifest.startLogZoom; logZoom > Constants::Fractal::ZOOM_MIN;
                 logZoom -= manifest.zoomIncrement) {
                if (++manifest.frameCount >= 1000000)
                    break;
            }
            std::string error;
            if (!manifest.valid(error)) {
                status = error;
                return false;
            }
            job.outputDirectory = directory;
            job.frames = createFrames(manifest, directory);
            job.running = true;
            if (!writeManifest(job, error)) {
                status = error;
                return false;
            }
            hostJob = std::move(job);
            network.broadcast(RenderPoolMessageType::JOB, hostJob->manifest.encode());
            network.broadcast(RenderPoolMessageType::JOB_STATE, encodeJobState(*hostJob));
            network.broadcast(RenderPoolMessageType::FRAME_STATES,
                              encodeFrameStates(hostJob->manifest.id, hostJob->frames));
            status = "Render-pool keyframe job started";
            return true;
        }

        bool resumeJob(const std::filesystem::path &directory) {
            RenderPoolJobManifest manifest;
            std::string error;
            if (!readManifest(directory, manifest, error)) {
                status = error;
                return false;
            }
            RenderPoolJob job;
            job.manifest = std::move(manifest);
            job.outputDirectory = directory;
            job.frames = createFrames(job.manifest, directory);
            job.running = job.completedCount() < job.manifest.frameCount;
            hostJob = std::move(job);
            network.broadcast(RenderPoolMessageType::JOB, hostJob->manifest.encode());
            network.broadcast(RenderPoolMessageType::JOB_STATE, encodeJobState(*hostJob));
            network.broadcast(RenderPoolMessageType::FRAME_STATES,
                              encodeFrameStates(hostJob->manifest.id, hostJob->frames));
            status = hostJob->running ? "Render-pool job resumed" : "Render-pool job is already complete";
            return true;
        }

        void broadcastFrameStates(const std::span<const RenderPoolFrame> frames) {
            if (!hostJob || frames.empty())
                return;
            network.broadcast(RenderPoolMessageType::FRAME_STATES, encodeFrameStates(hostJob->manifest.id, frames));
        }

        void broadcastFrameState(const RenderPoolFrame &frame) {
            broadcastFrameStates(std::span<const RenderPoolFrame>(&frame, 1));
        }

        std::optional<PoolTask> claimTask(const uint64_t workerId, const std::string &workerName) {
            if (!hostJob || !hostJob->running || hostJob->paused)
                return std::nullopt;
            for (const auto &frame: hostJob->frames) {
                if (frame.workerId == workerId &&
                    (frame.state == RenderPoolFrameState::ASSIGNED || frame.state == RenderPoolFrameState::RENDERING)) {
                    return PoolTask{.jobId = hostJob->manifest.id,
                                    .frameId = frame.id,
                                    .logZoom = frame.logZoom,
                                    .workerId = workerId,
                                    .workerName = workerName};
                }
            }
            for (auto &frame: hostJob->frames) {
                if (frame.state != RenderPoolFrameState::WAITING)
                    continue;
                frame.state = RenderPoolFrameState::ASSIGNED;
                frame.workerId = workerId;
                frame.workerName = workerName;
                ++frame.attempts;
                broadcastFrameState(frame);
                return PoolTask{.jobId = hostJob->manifest.id,
                                .frameId = frame.id,
                                .logZoom = frame.logZoom,
                                .workerId = workerId,
                                .workerName = workerName};
            }
            return std::nullopt;
        }

        void requeueWorker(const uint64_t workerId) {
            if (!hostJob)
                return;
            std::vector<RenderPoolFrame> changed;
            for (auto &frame: hostJob->frames) {
                if (frame.workerId == workerId && frame.state != RenderPoolFrameState::COMPLETE) {
                    frame.state = frame.attempts >= 3 ? RenderPoolFrameState::FAILED : RenderPoolFrameState::WAITING;
                    frame.error = "Worker disconnected";
                    frame.workerId = 0;
                    frame.workerName.clear();
                    changed.push_back(frame);
                }
            }
            broadcastFrameStates(changed);
        }

        bool acceptResult(const uint64_t workerId, const uint64_t jobId, const uint32_t frameId,
                          const std::span<const std::byte> mapBytes) {
            if (!hostJob || hostJob->manifest.id != jobId || frameId == 0 || frameId > hostJob->frames.size())
                return false;
            auto &frame = hostJob->frames[frameId - 1];
            if (frame.state == RenderPoolFrameState::COMPLETE)
                return true;
            if (frame.workerId != workerId) {
                frame.error = "Result came from a worker that did not own this frame";
                return false;
            }
            frame.state = RenderPoolFrameState::VERIFYING;
            broadcastFrameState(frame);
            const auto map = RFFDynamicMapBinary::decode(mapBytes);
            const auto internalExtent = RendererUtils::getInternalImageExtent(
                    {hostJob->manifest.windowWidth, hostJob->manifest.windowHeight},
                    hostJob->manifest.clarityMultiplier);
            if (!map.hasData() || map.getMatrix().getWidth() != internalExtent.width ||
                map.getMatrix().getHeight() != internalExtent.height ||
                std::abs(map.getLogZoom() - frame.logZoom) >= 0.0005f) {
                frame.error = "Worker returned a mismatched or damaged keyframe";
                frame.state = frame.attempts >= 3 ? RenderPoolFrameState::FAILED : RenderPoolFrameState::WAITING;
                frame.workerId = 0;
                broadcastFrameState(frame);
                return false;
            }

            const auto finalPath =
                    hostJob->outputDirectory / IOUtilities::fileNameFormat(frameId, Constants::File::EXT_DYNAMIC_MAP);
            const auto temporaryPath = finalPath.string() + ".partial";
            map.exportFile(temporaryPath);
            const auto check = RFFDynamicMapBinary::read(temporaryPath);
            if (!check.hasData()) {
                std::error_code ignored;
                std::filesystem::remove(temporaryPath, ignored);
                frame.error = "Could not verify the received keyframe on disk";
                frame.state = frame.attempts >= 3 ? RenderPoolFrameState::FAILED : RenderPoolFrameState::WAITING;
                frame.workerId = 0;
                broadcastFrameState(frame);
                return false;
            }
            std::error_code ec;
            std::filesystem::remove(finalPath, ec);
            ec.clear();
            std::filesystem::rename(temporaryPath, finalPath, ec);
            if (ec) {
                frame.error = "Could not install the received keyframe: " + ec.message();
                frame.state = RenderPoolFrameState::FAILED;
                broadcastFrameState(frame);
                return false;
            }
            RFFLocationBinary(frame.logZoom, hostJob->manifest.centerReal, hostJob->manifest.centerImag,
                              map.getMaxIteration())
                    .exportAsKeyframe(hostJob->outputDirectory, frameId);
            frame.state = RenderPoolFrameState::COMPLETE;
            frame.error.clear();
            broadcastFrameState(frame);
            if (hostJob->completedCount() == hostJob->manifest.frameCount) {
                hostJob->running = false;
                status = "All render-pool keyframes are complete";
            }
            network.broadcast(RenderPoolMessageType::JOB_STATE, encodeJobState(*hostJob));
            return true;
        }

        void sendTaskToPeer(const uint64_t peerId) {
            const auto worker = workers.find(peerId);
            const std::string workerName = worker == workers.end() ? "Worker" : worker->second.name;
            if (const auto task = claimTask(peerId, workerName)) {
                network.sendToPeer(peerId, RenderPoolMessageType::TASK, encodeTask(*task));
            } else {
                network.sendToPeer(peerId, RenderPoolMessageType::NO_TASK);
            }
        }

        void handleHostMessage(const RenderPoolNetworkEvent &event) {
            if (event.messageType == RenderPoolMessageType::REQUEST_TASK) {
                sendTaskToPeer(event.peerId);
                return;
            }
            if (event.messageType == RenderPoolMessageType::RESULT) {
                RenderPoolBinaryReader reader(event.payload);
                uint64_t jobId = 0;
                uint32_t frameId = 0;
                uint64_t length = 0;
                if (!reader.integer(jobId) || !reader.integer(frameId) || !reader.integer(length) ||
                    length != reader.remaining()) {
                    return;
                }
                std::vector<std::byte> mapBytes;
                if (reader.bytes(mapBytes, static_cast<size_t>(length)) && reader.finished())
                    acceptResult(event.peerId, jobId, frameId, mapBytes);
                return;
            }
            if (event.messageType == RenderPoolMessageType::WORKER_STATE) {
                uint64_t jobId = 0;
                uint32_t frameId = 0;
                RenderPoolFrameState state = RenderPoolFrameState::ASSIGNED;
                if (!decodeWorkerState(event.payload, jobId, frameId, state) || !hostJob ||
                    hostJob->manifest.id != jobId || frameId > hostJob->frames.size()) {
                    return;
                }
                auto &frame = hostJob->frames[frameId - 1];
                if (frame.workerId == event.peerId && frame.state != RenderPoolFrameState::COMPLETE &&
                    state == RenderPoolFrameState::RENDERING) {
                    frame.state = state;
                    broadcastFrameState(frame);
                }
            }
        }

        void handleWorkerMessage(const RenderPoolNetworkEvent &event) {
            if (event.messageType == RenderPoolMessageType::JOB) {
                RenderPoolJobManifest manifest;
                std::string error;
                if (!RenderPoolJobManifest::decode(event.payload, manifest, error)) {
                    status = error;
                    return;
                }
                workerJob = std::move(manifest);
                workerFrames = createWorkerFrames(*workerJob);
                workerCompletedFrames = 0;
                workerJobRunning = true;
                workerJobPaused = false;
                status = "Received render-pool keyframe job";
                return;
            }
            if (event.messageType == RenderPoolMessageType::TASK) {
                PoolTask task;
                if (decodeTask(event.payload, task) && workerJob && task.jobId == workerJob->id && !localTask)
                    queuedTask = std::move(task);
                return;
            }
            if (event.messageType == RenderPoolMessageType::NO_TASK) {
                status = workerJobRunning ? "Waiting for an available keyframe" : "The render-pool job is complete";
                return;
            }
            if (event.messageType == RenderPoolMessageType::JOB_STATE) {
                RenderPoolBinaryReader reader(event.payload);
                uint64_t jobId = 0;
                uint32_t total = 0;
                if (!reader.integer(jobId) || !reader.integer(workerCompletedFrames) || !reader.integer(total) ||
                    !reader.boolean(workerJobRunning) || !reader.boolean(workerJobPaused) || !reader.finished())
                    return;
                if (workerJob && workerJob->id == jobId)
                    workerJob->frameCount = total;
                return;
            }
            if (event.messageType == RenderPoolMessageType::FRAME_STATES) {
                uint64_t jobId = 0;
                std::vector<std::pair<uint32_t, RenderPoolFrameState>> updates;
                if (!decodeFrameStates(event.payload, jobId, updates) || !workerJob || workerJob->id != jobId)
                    return;
                if (workerFrames.size() != workerJob->frameCount)
                    workerFrames = createWorkerFrames(*workerJob);
                for (const auto &[frameId, state]: updates) {
                    if (frameId <= workerFrames.size())
                        workerFrames[frameId - 1].state = state;
                }
            }
        }

        void processNetworkEvents() {
            for (auto &event: network.takeEvents()) {
                switch (event.type) {
                    case RenderPoolNetworkEventType::LISTENING:
                        status = event.text;
                        break;
                    case RenderPoolNetworkEventType::DISCOVERING:
                        status = event.text;
                        break;
                    case RenderPoolNetworkEventType::PASSWORD_REQUIRED:
                        if (passwordInput[0] != '\0') {
                            network.submitPassword(passwordInput.data());
                            waitingForPassword = false;
                            status = "Authenticating";
                        } else {
                            waitingForPassword = true;
                            status = event.text;
                        }
                        break;
                    case RenderPoolNetworkEventType::AUTHENTICATED:
                        authenticated = true;
                        waitingForPassword = false;
                        status = "Connected to render pool";
                        break;
                    case RenderPoolNetworkEventType::PEER_AUTHENTICATED:
                        workers[event.peerId] = WorkerInfo{event.peerId, event.peerName};
                        status = event.peerName + " joined the render pool";
                        if (hostJob) {
                            network.sendToPeer(event.peerId, RenderPoolMessageType::JOB, hostJob->manifest.encode());
                            network.sendToPeer(event.peerId, RenderPoolMessageType::JOB_STATE,
                                               encodeJobState(*hostJob));
                            network.sendToPeer(event.peerId, RenderPoolMessageType::FRAME_STATES,
                                               encodeFrameStates(hostJob->manifest.id, hostJob->frames));
                        }
                        break;
                    case RenderPoolNetworkEventType::MESSAGE:
                        if (role == PoolRole::HOST)
                            handleHostMessage(event);
                        else if (role == PoolRole::WORKER)
                            handleWorkerMessage(event);
                        break;
                    case RenderPoolNetworkEventType::DISCONNECTED:
                        if (role == PoolRole::HOST) {
                            requeueWorker(event.peerId);
                            workers.erase(event.peerId);
                            status = event.peerName + " left the render pool";
                        } else {
                            authenticated = false;
                            status = event.text;
                        }
                        break;
                    case RenderPoolNetworkEventType::FAILURE:
                        status = event.text;
                        if (role == PoolRole::WORKER) {
                            authenticated = false;
                            waitingForPassword = false;
                        }
                        break;
                }
            }
        }

        void beginLocalTask(RFF2 &app, const RenderPoolJobManifest &manifest, PoolTask task) {
            if (!savedSettings) {
                savedSettings = app.getSettings();
                savedWindowExtent = app.getWindowContext().getSwapchain().getSwapchainExtent();
                localThreads = std::max(1U, app.getSettings().fractal.general.threads);
            }
            app.getAutoExplorer().stop();
            app.getState().cancel();
            app.beginRenderPoolNavigationLock();
            manifest.apply(app.getSettings(), task.logZoom, localThreads);
            app.getWindowContext().getWindow()->setResolution(static_cast<int>(manifest.windowWidth),
                                                              static_cast<int>(manifest.windowHeight));
            expectedRenderCount = app.getCompletedRenderCount() + 1;
            app.getRequests().requestResize({manifest.windowWidth, manifest.windowHeight});
            localTask = std::move(task);
            localPhase = LocalRenderPhase::RENDERING;
            if (role == PoolRole::HOST && hostJob && localTask->frameId <= hostJob->frames.size()) {
                auto &frame = hostJob->frames[localTask->frameId - 1];
                frame.state = RenderPoolFrameState::RENDERING;
                broadcastFrameState(frame);
            }
            if (role == PoolRole::WORKER)
                network.sendToServer(RenderPoolMessageType::WORKER_STATE,
                                     encodeWorkerState(*localTask, RenderPoolFrameState::RENDERING));
            status = std::format("Rendering keyframe {} at zoom E{:.3f}", localTask->frameId, localTask->logZoom);
        }

        void finishLocalTask(RFF2 &app) {
            if (!localTask)
                return;
            const PoolTask task = *localTask;
            if (!app.getLastRenderSucceeded()) {
                if (role == PoolRole::HOST && hostJob && task.frameId > 0 &&
                    task.frameId <= hostJob->frames.size()) {
                    auto &frame = hostJob->frames[task.frameId - 1];
                    frame.state = frame.attempts >= 3 ? RenderPoolFrameState::FAILED
                                                      : RenderPoolFrameState::WAITING;
                    frame.workerId = 0;
                    frame.workerName.clear();
                    frame.error = "Host rendering was interrupted";
                    broadcastFrameState(frame);
                }
                status = "Keyframe rendering was interrupted";
            } else if (role == PoolRole::WORKER && !authenticated) {
                ImGui::TextUnformatted(network.isRunning() ? "Connecting..." : "Not connected");
                if (ImGui::Button("Leave Pool", ImVec2(-FLT_MIN, 0)))
                    leave(app);
            } else {
                const auto map = app.generateMap();
                const auto bytes = map.encode();
                if (role == PoolRole::HOST) {
                    acceptResult(0, task.jobId, task.frameId, bytes);
                } else {
                    RenderPoolBinaryWriter result;
                    result.integer(task.jobId);
                    result.integer(task.frameId);
                    result.integer(static_cast<uint64_t>(bytes.size()));
                    result.bytes(bytes);
                    if (!network.sendToServer(RenderPoolMessageType::RESULT, result.view()))
                        status = "Could not return the completed keyframe to the host";
                }
            }
            localTask.reset();
            localPhase = LocalRenderPhase::IDLE;
        }

        void updateLocalRenderer(RFF2 &app) {
            if (localPhase == LocalRenderPhase::RENDERING && app.isIdleCompute() &&
                app.getCompletedRenderCount() >= expectedRenderCount) {
                finishLocalTask(app);
            }

            if (localPhase != LocalRenderPhase::IDLE)
                return;
            if (role == PoolRole::HOST && hostRenders && hostJob && hostJob->running && !hostJob->paused) {
                if (const auto task = claimTask(0, "Host"))
                    beginLocalTask(app, hostJob->manifest, *task);
                else if (savedSettings && hostJob->completedCount() == hostJob->manifest.frameCount)
                    restoreLocalState(app);
                return;
            }
            if (role == PoolRole::WORKER && authenticated && workerRenders && workerJob && workerJobRunning &&
                !workerJobPaused) {
                if (queuedTask) {
                    PoolTask task = std::move(*queuedTask);
                    queuedTask.reset();
                    beginLocalTask(app, *workerJob, std::move(task));
                } else {
                    const double now = app.getWindowContext().getWindow()->getTime();
                    if (now - lastTaskRequest >= TASK_REQUEST_INTERVAL_SECONDS) {
                        network.sendToServer(RenderPoolMessageType::REQUEST_TASK);
                        lastTaskRequest = now;
                    }
                }
                return;
            }
            if (savedSettings && !localTask &&
                ((role == PoolRole::WORKER && (!authenticated || !workerRenders || !workerJobRunning)) ||
                 (role == PoolRole::HOST && (!hostRenders || !hostJob || !hostJob->running)))) {
                restoreLocalState(app);
            }
        }

        static void drawJobSettings(const RenderPoolJobManifest &manifest) {
            const auto internalExtent = RendererUtils::getInternalImageExtent(
                    {manifest.windowWidth, manifest.windowHeight}, manifest.clarityMultiplier);
            ImGui::Text("Output resolution: %u x %u", manifest.windowWidth, manifest.windowHeight);
            ImGui::Text("Clarity: %.3fx", manifest.clarityMultiplier);
            ImGui::Text("Calculation resolution: %u x %u", internalExtent.width, internalExtent.height);
        }

        static void drawFrameGrid(const std::span<const RenderPoolFrame> frames) {
            if (frames.empty())
                return;
            ImGui::TextUnformatted("Keyframe progress");
            constexpr float cellSize = 13;
            const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            const int columns = std::max(1, static_cast<int>(available / (cellSize + 2)));
            const int rows =
                    static_cast<int>((frames.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
            ImGuiListClipper clipper;
            clipper.Begin(rows, cellSize + ImGui::GetStyle().ItemSpacing.y);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    for (int column = 0; column < columns; ++column) {
                        const size_t index = static_cast<size_t>(row) * static_cast<size_t>(columns) + column;
                        if (index >= frames.size())
                            break;
                        const auto &frame = frames[index];
                        ImGui::PushID(static_cast<int>(index));
                        const ImVec2 topLeft = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##frame", {cellSize, cellSize});
                        const ImVec2 center = {topLeft.x + cellSize * 0.5f, topLeft.y + cellSize * 0.5f};
                        ImDrawList *drawList = ImGui::GetWindowDrawList();
                        drawList->AddCircleFilled(center, cellSize * 0.38f,
                                                  ImGui::ColorConvertFloat4ToU32(frameColor(frame.state)));
                        if (ImGui::IsItemHovered()) {
                            drawList->AddCircle(center, cellSize * 0.46f, IM_COL32(255, 255, 255, 220));
                            ImGui::BeginTooltip();
                            ImGui::Text("Keyframe %u", frame.id);
                            ImGui::Text("Zoom E%.4f", frame.logZoom);
                            ImGui::Text("%s", frameStateText(frame.state));
                            if (!frame.workerName.empty())
                                ImGui::Text("Worker: %s", frame.workerName.c_str());
                            if (frame.attempts > 0)
                                ImGui::Text("Attempts: %u", frame.attempts);
                            if (!frame.error.empty())
                                ImGui::TextWrapped("%s", frame.error.c_str());
                            ImGui::EndTooltip();
                        }
                        if (column + 1 < columns && index + 1 < frames.size())
                            ImGui::SameLine(0, 2);
                        ImGui::PopID();
                    }
                }
            }
            clipper.End();
        }

        void drawPanel(RFF2 &app) {
            if (role == PoolRole::NONE) {
                int mode = static_cast<int>(connectionMode);
                if (ImGui::Combo("Connection", &mode, "Internet\0LAN (automatic)\0"))
                    connectionMode = static_cast<PoolConnectionMode>(mode);
                if (connectionMode == PoolConnectionMode::INTERNET) {
                    ImGui::InputText("ID", poolIdInput.data(), poolIdInput.size(), ImGuiInputTextFlags_CharsDecimal);
                    ImGui::InputText("Password", passwordInput.data(), passwordInput.size(),
                                     ImGuiInputTextFlags_Password);
                }
                ImGui::InputText("Computer Name", workerNameInput.data(), workerNameInput.size());
                if (ImGui::Button("Join", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 3, 0)))
                    joinPool();
                ImGui::SameLine();
                if (ImGui::Button("Start", ImVec2(-FLT_MIN, 0)))
                    startHost();
                ImGui::Separator();
                ImGui::TextWrapped("%s", status.c_str());
                return;
            }

            if (waitingForPassword) {
                ImGui::InputText("Password", passwordInput.data(), passwordInput.size(), ImGuiInputTextFlags_Password);
                if (ImGui::Button("Authenticate", ImVec2(-FLT_MIN, 0))) {
                    network.submitPassword(passwordInput.data());
                    status = "Authenticating";
                }
            } else if (role == PoolRole::HOST) {
                if (connectionMode == PoolConnectionMode::INTERNET) {
                    if (poolId.empty()) {
                        ImGui::TextUnformatted("Pool ID: preparing UPnP...");
                    } else {
                        ImGui::Text("Pool ID: %s", poolId.c_str());
                        if (ImGui::Button("Copy ID", ImVec2(-FLT_MIN, 0))) {
                            ImGui::SetClipboardText(poolId.c_str());
                            status = "Pool ID copied to the clipboard";
                        }
                    }
                    if (!upnpStatus.empty())
                        ImGui::TextWrapped("%s", upnpStatus.c_str());
                } else {
                    ImGui::TextUnformatted("LAN automatic discovery is active");
                }
                ImGui::Text("Connected workers: %zu", workers.size());
                ImGui::Checkbox("Host Also Renders", &hostRenders);

                if (!hostJob) {
                    if (ImGui::Button("Start Keyframe Job", ImVec2(-FLT_MIN, 0))) {
                        if (const auto directory = IOUtilities::ioDirectoryDialog())
                            createJobFromCurrent(app, *directory);
                    }
                    if (ImGui::Button("Resume Keyframe Job", ImVec2(-FLT_MIN, 0))) {
                        if (const auto directory = IOUtilities::ioDirectoryDialog())
                            resumeJob(*directory);
                    }
                } else {
                    drawJobSettings(hostJob->manifest);
                    const uint32_t completed = hostJob->completedCount();
                    ImGui::Text("Keyframes: %u / %u", completed, hostJob->manifest.frameCount);
                    ImGui::ProgressBar(static_cast<float>(completed) /
                                       static_cast<float>(hostJob->manifest.frameCount));
                    ImGui::Checkbox("Pause New Assignments", &hostJob->paused);
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        network.broadcast(RenderPoolMessageType::JOB_STATE, encodeJobState(*hostJob));
                    drawFrameGrid(hostJob->frames);
                    const bool hasFailed = std::ranges::any_of(hostJob->frames, [](const RenderPoolFrame &frame) {
                        return frame.state == RenderPoolFrameState::FAILED;
                    });
                    if (hasFailed && ImGui::Button("Retry Failed Keyframes", ImVec2(-FLT_MIN, 0))) {
                        for (auto &frame: hostJob->frames) {
                            if (frame.state == RenderPoolFrameState::FAILED) {
                                frame.state = RenderPoolFrameState::WAITING;
                                frame.workerId = 0;
                                frame.workerName.clear();
                                frame.error.clear();
                            }
                        }
                        hostJob->running = true;
                        hostJob->paused = false;
                        network.broadcast(RenderPoolMessageType::JOB_STATE, encodeJobState(*hostJob));
                        network.broadcast(RenderPoolMessageType::FRAME_STATES,
                                          encodeFrameStates(hostJob->manifest.id, hostJob->frames));
                    }
                    if (!hostJob->running && ImGui::Button("Close Completed Job", ImVec2(-FLT_MIN, 0)))
                        hostJob.reset();
                }
            } else {
                ImGui::Checkbox("Render Assigned Keyframes", &workerRenders);
                if (workerJob) {
                    drawJobSettings(*workerJob);
                    ImGui::Text("Keyframes: %u / %u", workerCompletedFrames, workerJob->frameCount);
                    ImGui::ProgressBar(static_cast<float>(workerCompletedFrames) /
                                       static_cast<float>(workerJob->frameCount));
                    if (localTask)
                        ImGui::Text("Rendering %u at E%.3f", localTask->frameId, localTask->logZoom);
                    drawFrameGrid(workerFrames);
                }
            }

            if (ImGui::Button("Leave Pool", ImVec2(-FLT_MIN, 0)))
                leave(app);
            ImGui::Separator();
            ImGui::TextWrapped("%s", status.c_str());
        }
    };

    RenderPool::RenderPool() : impl(std::make_unique<Impl>()) {}
    RenderPool::~RenderPool() = default;

    void RenderPool::update(RFF2 &app) {
        impl->pollUpnp();
        impl->processNetworkEvents();
        impl->updateLocalRenderer(app);
    }

    void RenderPool::renderPanel(RFF2 &app) { impl->drawPanel(app); }

    void RenderPool::shutdown(RFF2 *app) {
        if (app != nullptr && impl->savedSettings)
            impl->restoreLocalState(*app);
        impl->network.stop();
        impl->stopUpnp();
        impl->clearConnectionState();
    }

    bool RenderPool::isActive() const { return impl->role != PoolRole::NONE; }

    bool RenderPool::ownsNavigation() const { return impl->savedSettings.has_value() || impl->localTask.has_value(); }
} // namespace merutilm::rff2
