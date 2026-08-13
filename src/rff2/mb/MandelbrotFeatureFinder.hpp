#pragma once

#include <cstdint>
#include <optional>

#include "../calc/complex.hpp"
#include "MB2RenderData.hpp"

namespace merutilm::rff2 {
    class MandelbrotFeatureFinder final {
    public:
        enum class Kind : uint8_t {
            PERIODIC,
            MISIUREWICZ,
        };

        struct Result {
            complex<dex> offsetFromReference = complex<dex>::ZERO;
            Kind kind = Kind::PERIODIC;
            uint64_t preperiod = 0;
            uint64_t period = 0;
            dex estimatedSize = dex::ZERO;
        };

        [[nodiscard]] static std::optional<Result>
        find(const MB2RenderDataBase &data, const complex<dex> &cursorOffsetFromReference, dex searchRadius);
    };
} // namespace merutilm::rff2
