//
// Created by Merutilm on 2025-05-08.
//

#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "RFFBinary.h"
#include "../data/Matrix.h"

namespace merutilm::rff2 {
    class RFFDynamicMapBinary final : public RFFBinary{
        uint64_t period;
        uint64_t maxIteration;
        Matrix<double> iterations;

    public:
        static const RFFDynamicMapBinary DEFAULT;

        RFFDynamicMapBinary(float logZoom, uint64_t period, uint64_t maxIteration, Matrix<double> iterations);

        [[nodiscard]] bool hasData() const override;

        [[nodiscard]] static RFFDynamicMapBinary read(const std::filesystem::path &path);

        [[nodiscard]] static RFFDynamicMapBinary decode(std::span<const std::byte> bytes);

        [[nodiscard]] std::vector<std::byte> encode() const;

        [[nodiscard]] static RFFDynamicMapBinary readByID(const std::filesystem::path &dir, uint32_t id);

        void exportAsKeyframe(const std::filesystem::path &dir) const override;

        void exportAsKeyframe(const std::filesystem::path &dir, uint32_t id) const;

        void exportFile(const std::filesystem::path &path) const override;

        [[nodiscard]] uint64_t getPeriod() const;

        [[nodiscard]] uint64_t getMaxIteration() const;

        [[nodiscard]] const Matrix<double> &getMatrix() const;
    };

}
