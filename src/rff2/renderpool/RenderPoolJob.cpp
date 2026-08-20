#include "RenderPoolJob.hpp"

#include <algorithm>
#include <cmath>

#include "RenderPoolBinary.hpp"
#include "../mb/MB2Perturbator.h"

namespace merutilm::rff2 {

    std::vector<std::byte> RenderPoolJobManifest::encode() const {
        RenderPoolBinaryWriter writer;
        writer.integer(FORMAT_VERSION);
        writer.integer(id);
        writer.integer(windowWidth);
        writer.integer(windowHeight);
        writer.floating(startLogZoom);
        writer.floating(zoomIncrement);
        writer.floating(clarityMultiplier);
        writer.string(centerReal);
        writer.string(centerImag);
        writer.floating(bailout);
        writer.boolean(useParallelReference);
        writer.integer(referenceSynchronizationInterval);
        writer.integer(referenceSynchronizationRadiusPower);
        writer.integer(compressCriteria);
        writer.integer(compressionThresholdPower);
        writer.boolean(useSeriesApproximation);
        writer.integer(appliedTermsCount);
        writer.integer(validatedTermsCount);
        writer.floating(seriesApproximationEpsilonPower);
        writer.integer(minimumSkippedReference);
        writer.integer(maximumMultiplierBetweenLevel);
        writer.floating(approximationEpsilonPower);
        writer.integer(approximationSelectionMethod);
        writer.boolean(compressApproximation);
        writer.boolean(parallelizeApproximation);
        writer.integer(maxIteration);
        writer.integer(decimalizeIterationMethod);
        writer.boolean(autoMaxIteration);
        writer.integer(interiorDetectRadiusPower);
        writer.integer(autoIterationMultiplier);
        writer.boolean(absoluteIterationMode);
        writer.integer(frameCount);
        return writer.take();
    }

    bool RenderPoolJobManifest::decode(const std::span<const std::byte> bytes, RenderPoolJobManifest &result,
                                       std::string &error) {
        RenderPoolBinaryReader reader(bytes);
        uint16_t version = 0;
        if (!reader.integer(version) || version != FORMAT_VERSION || !reader.integer(result.id) ||
            !reader.integer(result.windowWidth) || !reader.integer(result.windowHeight) ||
            !reader.floating(result.startLogZoom) || !reader.floating(result.zoomIncrement) ||
            !reader.floating(result.clarityMultiplier) || !reader.string(result.centerReal) ||
            !reader.string(result.centerImag) || !reader.floating(result.bailout) ||
            !reader.boolean(result.useParallelReference) ||
            !reader.integer(result.referenceSynchronizationInterval) ||
            !reader.integer(result.referenceSynchronizationRadiusPower) ||
            !reader.integer(result.compressCriteria) || !reader.integer(result.compressionThresholdPower) ||
            !reader.boolean(result.useSeriesApproximation) || !reader.integer(result.appliedTermsCount) ||
            !reader.integer(result.validatedTermsCount) ||
            !reader.floating(result.seriesApproximationEpsilonPower) ||
            !reader.integer(result.minimumSkippedReference) ||
            !reader.integer(result.maximumMultiplierBetweenLevel) ||
            !reader.floating(result.approximationEpsilonPower) ||
            !reader.integer(result.approximationSelectionMethod) ||
            !reader.boolean(result.compressApproximation) ||
            !reader.boolean(result.parallelizeApproximation) || !reader.integer(result.maxIteration) ||
            !reader.integer(result.decimalizeIterationMethod) || !reader.boolean(result.autoMaxIteration) ||
            !reader.integer(result.interiorDetectRadiusPower) || !reader.integer(result.autoIterationMultiplier) ||
            !reader.boolean(result.absoluteIterationMode) || !reader.integer(result.frameCount) || !reader.finished()) {
            error = version == FORMAT_VERSION ? "Incomplete render-pool job" : "Unsupported render-pool job version";
            return false;
        }
        return result.valid(error);
    }

    bool RenderPoolJobManifest::valid(std::string &error) const {
        if (id == 0 || windowWidth == 0 || windowHeight == 0 || frameCount == 0) {
            error = "Render-pool job dimensions or identity are invalid";
            return false;
        }
        if (!std::isfinite(startLogZoom) || !std::isfinite(zoomIncrement) || zoomIncrement <= 0 ||
            !std::isfinite(clarityMultiplier) || clarityMultiplier <= 0 || !std::isfinite(bailout) || bailout <= 0) {
            error = "Render-pool job contains invalid numeric settings";
            return false;
        }
        if (centerReal.empty() || centerImag.empty() || centerReal.size() > (1U << 20) ||
            centerImag.size() > (1U << 20)) {
            error = "Render-pool job center is invalid";
            return false;
        }
        if (approximationSelectionMethod > static_cast<uint8_t>(FrtMPASelectionMethod::HIGHEST) ||
            decimalizeIterationMethod > static_cast<uint8_t>(FrtDecimalizeIterationMethod::LOG_LOG)) {
            error = "Render-pool job contains unsupported calculation modes";
            return false;
        }
        return true;
    }

    void RenderPoolJobManifest::apply(Settings &settings, const float logZoom, const uint32_t localThreads) const {
        auto &fractal = settings.fractal;
        fractal.general.bailout = bailout;
        fractal.general.logZoom = logZoom;
        fractal.general.threads = std::max(1U, localThreads);
        fractal.reference.center = fixed_point_complex_i1(centerReal, centerImag,
                                                          Perturbator::logZoomToExp10(logZoom));
        fractal.reference.useParallelRefCalculation = useParallelReference;
        fractal.reference.sync.referenceSynchronizationInterval = referenceSynchronizationInterval;
        fractal.reference.sync.referenceSynchronizationRadiusPower = referenceSynchronizationRadiusPower;
        fractal.reference.compression.compressCriteria = compressCriteria;
        fractal.reference.compression.compressionThresholdPower = compressionThresholdPower;
        fractal.reference.reuse = false;
        fractal.sa.use = useSeriesApproximation;
        fractal.sa.appliedTermsCount = appliedTermsCount;
        fractal.sa.validatedTermsCount = validatedTermsCount;
        fractal.sa.epsilonPower = seriesApproximationEpsilonPower;
        fractal.mpa.minSkipReference = minimumSkippedReference;
        fractal.mpa.maxMultiplierBetweenLevel = maximumMultiplierBetweenLevel;
        fractal.mpa.epsilonPower = approximationEpsilonPower;
        fractal.mpa.mpaSelectionMethod = static_cast<FrtMPASelectionMethod>(approximationSelectionMethod);
        fractal.mpa.useCompress = compressApproximation;
        fractal.mpa.useParallelization = parallelizeApproximation;
        fractal.perturb.maxIteration = maxIteration;
        fractal.perturb.decimalizeIterationMethod =
                static_cast<FrtDecimalizeIterationMethod>(decimalizeIterationMethod);
        fractal.perturb.autoMaxIteration = autoMaxIteration;
        fractal.perturb.interiorDetectRadiusPower = interiorDetectRadiusPower;
        fractal.perturb.autoIterationMultiplier = autoIterationMultiplier;
        fractal.perturb.absoluteIterationMode = absoluteIterationMode;
        settings.render.clarityMultiplier = clarityMultiplier;
        settings.video.data.isStatic = false;
    }

    uint32_t RenderPoolJob::completedCount() const {
        return static_cast<uint32_t>(std::ranges::count_if(frames, [](const RenderPoolFrame &frame) {
            return frame.state == RenderPoolFrameState::COMPLETE;
        }));
    }
}
