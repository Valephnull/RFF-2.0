//
// Created by Merutilm on 2025-05-08.
//

#include "RFFDynamicMapBinary.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#include "vulkan_helper/util/BufferImageUtils.hpp"
#include "vulkan_helper/base/logger.hpp"
#include "../ui/IOUtilities.h"
#include "../constants/Constants.hpp"

namespace merutilm::rff2 {

    namespace {
        template<typename T>
        void appendValue(std::vector<std::byte> &bytes, const T &value) {
            const auto offset = bytes.size();
            bytes.resize(offset + sizeof(T));
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        template<typename T>
        bool readValue(const std::span<const std::byte> bytes, size_t &offset, T &value) {
            if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
                return false;
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            offset += sizeof(T);
            return true;
        }
    }

    inline const RFFDynamicMapBinary RFFDynamicMapBinary::DEFAULT = RFFDynamicMapBinary(0, 0, 0, Matrix<double>(0, 0));

    RFFDynamicMapBinary::RFFDynamicMapBinary(const float logZoom, const uint64_t period, const uint64_t maxIteration,
                                  Matrix<double> iterations) : RFFBinary(logZoom), period(period), maxIteration(maxIteration),
                                                               iterations(std::move(iterations)) {
    }


    bool RFFDynamicMapBinary::hasData() const {
        return iterations.getWidth() > 0;
    }


    RFFDynamicMapBinary RFFDynamicMapBinary::read(const std::filesystem::path &path) {
        if (!std::filesystem::exists(path)) {
            return DEFAULT;
        }
        std::ifstream in(path, std::ios::in | std::ios::binary | std::ios::ate);

        if (!in.is_open()) {
            return DEFAULT;
        }
        const auto length = in.tellg();
        if (length <= 0 || static_cast<uint64_t>(length) > std::numeric_limits<size_t>::max())
            return DEFAULT;
        in.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<size_t>(length));
        in.read(reinterpret_cast<char *>(bytes.data()), length);
        if (!in)
            return DEFAULT;
        return decode(bytes);
    }

    RFFDynamicMapBinary RFFDynamicMapBinary::decode(const std::span<const std::byte> bytes) {
        size_t offset = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        float logZoom = 0;
        uint64_t period = 0;
        uint64_t maxIteration = 0;
        if (!readValue(bytes, offset, width) || !readValue(bytes, offset, height) ||
            !readValue(bytes, offset, logZoom) || !readValue(bytes, offset, period) ||
            !readValue(bytes, offset, maxIteration) || width == 0 || height == 0 ||
            !std::isfinite(logZoom)) {
            return DEFAULT;
        }

        const size_t elementCount = static_cast<size_t>(width) * height;
        if (elementCount > (bytes.size() - offset) / sizeof(double) ||
            bytes.size() - offset != elementCount * sizeof(double)) {
            return DEFAULT;
        }
        std::vector<double> iterations(elementCount);
        std::memcpy(iterations.data(), bytes.data() + offset, elementCount * sizeof(double));
        return RFFDynamicMapBinary(logZoom, period, maxIteration, Matrix(width, height, iterations));
    }

    std::vector<std::byte> RFFDynamicMapBinary::encode() const {
        if (!hasData())
            return {};
        const auto &canvas = iterations.getCanvas();
        std::vector<std::byte> bytes;
        bytes.reserve(sizeof(uint16_t) * 2 + sizeof(float) + sizeof(uint64_t) * 2 +
                      canvas.size() * sizeof(double));
        appendValue(bytes, iterations.getWidth());
        appendValue(bytes, iterations.getHeight());
        appendValue(bytes, getLogZoom());
        appendValue(bytes, period);
        appendValue(bytes, maxIteration);
        const auto offset = bytes.size();
        bytes.resize(offset + canvas.size() * sizeof(double));
        std::memcpy(bytes.data() + offset, canvas.data(), canvas.size() * sizeof(double));
        return bytes;
    }

    RFFDynamicMapBinary RFFDynamicMapBinary::readByID(const std::filesystem::path& dir, const uint32_t id) {
        return read(dir / IOUtilities::fileNameFormat(id, Constants::File::EXT_DYNAMIC_MAP));
    }


    void RFFDynamicMapBinary::exportAsKeyframe(const std::filesystem::path &dir) const {
        exportFile(IOUtilities::generateFilename(dir, Constants::File::EXT_DYNAMIC_MAP, nullptr));
    }

    void RFFDynamicMapBinary::exportAsKeyframe(const std::filesystem::path &dir, const uint32_t id) const {
        exportFile(dir / IOUtilities::fileNameFormat(id, Constants::File::EXT_DYNAMIC_MAP));
    }

    void RFFDynamicMapBinary::exportFile(const std::filesystem::path &path) const {
        if (std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc); out.is_open()) {
            const auto bytes = encode();
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
        } else {
            vkh::logger::log("ERROR : Cannot save file");
        }
    }


    uint64_t RFFDynamicMapBinary::getPeriod() const {
        return period;
    }

    uint64_t RFFDynamicMapBinary::getMaxIteration() const {
        return maxIteration;
    }

    const Matrix<double> &RFFDynamicMapBinary::getMatrix() const {
        return iterations;
    }
}
