#include "MandelbrotFeatureFinder.hpp"

#include <algorithm>
#include <vector>

namespace merutilm::rff2 {
    namespace {
        constexpr double NEAR_LINEAR_RADIUS_SCALE_SQUARED = 0.0625;
        constexpr double MISIUREWICZ_CAPTURE_SCALE_SQUARED = 0.25;
        constexpr double MIN_DISTINCT_MAGNITUDE_SCALE = 0x1.0p-24;
        constexpr double NEWTON_RELATIVE_TOLERANCE_SQUARED = 1.5e-14;
        constexpr double NEWTON_RADIUS_TOLERANCE_SQUARED = 1.0e-12;

        struct OrbitPoint {
            complex<dex> z = complex<dex>::ZERO;
            complex<dex> derivative = complex<dex>::ZERO;
            dex magnitudeSquared = dex::ZERO;
            uint64_t iteration = 0;
        };

        struct Detection {
            uint64_t preperiod = 0;
            uint64_t endIteration = 0;
        };

        struct FixedEvaluation {
            complex<dex> difference = complex<dex>::ZERO;
            complex<dex> derivativeDifference = complex<dex>::ZERO;
            bool completed = false;
        };

        [[nodiscard]] complex<dex> normalized(const complex<dex> value) { return value.try_normalized_value(); }

        [[nodiscard]] complex<dex> divideComplex(const complex<dex> numerator, const complex<dex> denominator) {
            const dex denominatorNorm = denominator.norm_sqr();
            if (denominatorNorm.is_zero())
                return complex<dex>::ZERO;
            return normalized({(numerator.re * denominator.re + numerator.im * denominator.im) / denominatorNorm,
                               (numerator.im * denominator.re - numerator.re * denominator.im) / denominatorNorm});
        }

        template<Number Num>
        [[nodiscard]] complex<dex> toDex(const complex<Num> value) {
            return {dex(value.re), dex(value.im)};
        }

        template<Number Num>
        class OrbitEvaluator final {
            const MB2Reference<Num> &reference;
            complex<dex> dc;
            uint64_t referenceIteration = 0;
            uint64_t iteration = 0;
            complex<dex> dz = complex<dex>::ZERO;
            complex<dex> z = complex<dex>::ZERO;
            complex<dex> derivative = complex<dex>::ZERO;

        public:
            OrbitEvaluator(const MB2Reference<Num> &reference, const complex<dex> dc) : reference(reference), dc(dc) {}

            [[nodiscard]] bool advance() {
                derivative = normalized(z * derivative * dex(2.0) + complex<dex>::ONE);

                const complex<dex> referenceZ = toDex(reference.orbit(referenceIteration));
                dz = normalized((referenceZ * dex(2.0) + dz) * dz + dc);
                ++referenceIteration;
                ++iteration;
                z = normalized(toDex(reference.orbit(referenceIteration)) + dz);

                if (referenceIteration == reference.longestPeriod() || z.norm_sqr() < dz.norm_sqr()) {
                    referenceIteration = 0;
                    dz = z;
                }
                return z.norm_sqr() <= dex(4096.0);
            }

            [[nodiscard]] uint64_t getIteration() const { return iteration; }
            [[nodiscard]] const complex<dex> &getZ() const { return z; }
            [[nodiscard]] const complex<dex> &getDerivative() const { return derivative; }
        };

        template<Number Num>
        [[nodiscard]] std::optional<Detection> detectPeriod(const MB2Reference<Num> &reference, const complex<dex> dc,
                                                            const dex radiusSquared, const uint64_t maximumIterations,
                                                            const std::stop_token &stopToken) {
            OrbitEvaluator evaluator(reference, dc);
            std::vector<OrbitPoint> misiurewiczStack;
            misiurewiczStack.reserve(static_cast<size_t>(std::min<uint64_t>(maximumIterations, 4096)));
            dex nearLinearRadiusSquared = radiusSquared;
            const dex captureRadiusSquared = radiusSquared * dex(MISIUREWICZ_CAPTURE_SCALE_SQUARED);

            const auto captured = [&](const OrbitPoint &point, const complex<dex> z, const complex<dex> derivative,
                                      const dex magnitudeSquared) {
                const dex separation = (z - point.z).norm_sqr();
                const dex derivativeSeparation = (derivative - point.derivative).norm_sqr();
                return separation < captureRadiusSquared * derivativeSeparation &&
                       separation > magnitudeSquared * dex(MIN_DISTINCT_MAGNITUDE_SCALE);
            };

            while (!stopToken.stop_requested() && evaluator.getIteration() < maximumIterations && evaluator.advance()) {
                const complex<dex> z = evaluator.getZ();
                const complex<dex> derivative = evaluator.getDerivative();
                const dex magnitudeSquared = z.norm_sqr();
                const dex derivativeMagnitudeSquared = derivative.norm_sqr();
                const uint64_t iteration = evaluator.getIteration();

                if (magnitudeSquared < nearLinearRadiusSquared * derivativeMagnitudeSquared)
                    return Detection{0, iteration};

                if (!misiurewiczStack.empty()) {
                    if (misiurewiczStack.back().magnitudeSquared > magnitudeSquared) {
                        OrbitPoint point;
                        do {
                            point = misiurewiczStack.back();
                            misiurewiczStack.pop_back();
                        } while (!misiurewiczStack.empty() &&
                                 misiurewiczStack.back().magnitudeSquared > magnitudeSquared);
                        if (captured(point, z, derivative, magnitudeSquared))
                            return Detection{point.iteration, iteration};
                    }
                    if (!misiurewiczStack.empty() &&
                        captured(misiurewiczStack.back(), z, derivative, magnitudeSquared)) {
                        return Detection{misiurewiczStack.back().iteration, iteration};
                    }
                }

                misiurewiczStack.push_back({z, derivative, magnitudeSquared, iteration});
                if (!derivativeMagnitudeSquared.is_zero() &&
                    magnitudeSquared * dex(NEAR_LINEAR_RADIUS_SCALE_SQUARED) <
                            nearLinearRadiusSquared * derivativeMagnitudeSquared) {
                    nearLinearRadiusSquared =
                            magnitudeSquared * dex(NEAR_LINEAR_RADIUS_SCALE_SQUARED) / derivativeMagnitudeSquared;
                }
            }
            return std::nullopt;
        }

        template<Number Num>
        [[nodiscard]] FixedEvaluation evaluateFixed(const MB2Reference<Num> &reference, const complex<dex> dc,
                                                    const uint64_t preperiod, const uint64_t endIteration,
                                                    const std::stop_token &stopToken) {
            OrbitEvaluator evaluator(reference, dc);
            complex<dex> preperiodZ = complex<dex>::ZERO;
            complex<dex> preperiodDerivative = complex<dex>::ZERO;

            while (evaluator.getIteration() < endIteration) {
                if (stopToken.stop_requested())
                    return {};
                if (!evaluator.advance())
                    return {};
                if (evaluator.getIteration() == preperiod) {
                    preperiodZ = evaluator.getZ();
                    preperiodDerivative = evaluator.getDerivative();
                }
            }
            return {evaluator.getZ() - preperiodZ, evaluator.getDerivative() - preperiodDerivative, true};
        }

        template<Number Num>
        [[nodiscard]] std::optional<MandelbrotFeatureFinder::Result>
        findWithReference(const MB2RenderData<Num> &data, const complex<dex> cursorOffsetFromReference,
                          const dex searchRadius, const std::stop_token &stopToken) {
            const MB2Reference<Num> *reference = data.reference.get();
            if (stopToken.stop_requested() || !reference || reference->longestPeriod() == 0 ||
                !(searchRadius > dex::ZERO))
                return std::nullopt;

            const uint64_t maximumIterations = data.fractalSettings.perturb.maxIteration;
            if (maximumIterations == 0)
                return std::nullopt;
            const dex radiusSquared = searchRadius * searchRadius;
            const std::optional<Detection> detection =
                    detectPeriod(*reference, cursorOffsetFromReference, radiusSquared, maximumIterations, stopToken);
            if (!detection || detection->endIteration <= detection->preperiod)
                return std::nullopt;

            complex<dex> refined = cursorOffsetFromReference;
            bool converged = false;
            std::vector<complex<dex>> refinementHistory;
            while (true) {
                if (stopToken.stop_requested())
                    return std::nullopt;
                const FixedEvaluation evaluation =
                        evaluateFixed(*reference, refined, detection->preperiod, detection->endIteration, stopToken);
                if (!evaluation.completed || evaluation.derivativeDifference.norm_sqr().is_zero())
                    return std::nullopt;

                const complex<dex> correction = divideComplex(evaluation.difference, evaluation.derivativeDifference);
                if (correction.is_zero()) {
                    converged = true;
                    break;
                }
                const complex<dex> nextRefined = normalized(refined - correction);
                const dex correctionMagnitude = correction.norm_sqr();
                if (correctionMagnitude < nextRefined.norm_sqr() * dex(NEWTON_RELATIVE_TOLERANCE_SQUARED) ||
                    correctionMagnitude < radiusSquared * dex(NEWTON_RADIUS_TOLERANCE_SQUARED)) {
                    refined = nextRefined;
                    converged = true;
                    break;
                }

                // There is no fixed pass limit. Reject divergent motion or an exact
                // finite-precision cycle so a pathological candidate cannot trap the UI.
                if ((nextRefined - cursorOffsetFromReference).norm_sqr() > radiusSquared ||
                    std::ranges::find(refinementHistory, nextRefined) != refinementHistory.end()) {
                    return std::nullopt;
                }
                refinementHistory.push_back(refined);
                refined = nextRefined;
            }

            if (!converged || (refined - cursorOffsetFromReference).norm_sqr() > radiusSquared)
                return std::nullopt;

            const FixedEvaluation validation =
                    evaluateFixed(*reference, refined, detection->preperiod, detection->endIteration, stopToken);
            const dex derivativeMagnitudeSquared = validation.derivativeDifference.norm_sqr();
            if (!validation.completed || derivativeMagnitudeSquared.is_zero())
                return std::nullopt;

            return MandelbrotFeatureFinder::Result{
                    refined,
                    detection->preperiod == 0 ? MandelbrotFeatureFinder::Kind::PERIODIC
                                              : MandelbrotFeatureFinder::Kind::MISIUREWICZ,
                    detection->preperiod,
                    detection->endIteration - detection->preperiod,
                    dex::ONE / dex::sqrt(derivativeMagnitudeSquared),
            };
        }
    } // namespace

    std::optional<MandelbrotFeatureFinder::Result>
    MandelbrotFeatureFinder::find(const MB2RenderDataBase &data, const complex<dex> &cursorOffsetFromReference,
                                  const dex searchRadius, const std::stop_token stopToken) {
        if (const auto *light = dynamic_cast<const LightMB2RenderData *>(&data))
            return findWithReference(*light, cursorOffsetFromReference, searchRadius, stopToken);
        if (const auto *deep = dynamic_cast<const DeepMB2RenderData *>(&data))
            return findWithReference(*deep, cursorOffsetFromReference, searchRadius, stopToken);
        return std::nullopt;
    }
} // namespace merutilm::rff2
