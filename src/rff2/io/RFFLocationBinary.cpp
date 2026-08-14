//
// Created by Merutilm on 2025-06-25.
//

#include "RFFLocationBinary.h"

#include <cmath>
#include <utility>

#include "../constants/FileConstants.hpp"
#include "../ui/IOUtilities.h"
#include "vulkan_helper/base/logger.hpp"

namespace merutilm::rff2 {
    namespace {
        constexpr uint64_t MAX_COORDINATE_TEXT_LENGTH = 1U << 20;

        template<typename T>
        [[nodiscard]] bool readExact(std::ifstream &input, T &value) {
            input.read(reinterpret_cast<char *>(&value), sizeof(T));
            return static_cast<bool>(input);
        }

        [[nodiscard]] bool readText(std::ifstream &input, std::string &value) {
            uint64_t length = 0;
            if (!readExact(input, length) || length == 0 || length > MAX_COORDINATE_TEXT_LENGTH)
                return false;
            value.resize(static_cast<size_t>(length));
            input.read(value.data(), static_cast<std::streamsize>(length));
            return static_cast<bool>(input);
        }
    } // namespace

    inline const RFFLocationBinary RFFLocationBinary::DEFAULT = RFFLocationBinary(0, "", "", 0);

    RFFLocationBinary::RFFLocationBinary(const float logZoom, std::string real, std::string imag,
                             const uint64_t maxIteration) : RFFBinary(logZoom), real(std::move(real)), imag(std::move(imag)),
                                                      maxIteration(maxIteration) {
    }

    RFFLocationBinary RFFLocationBinary::read(const std::filesystem::path &path) {
        if (!std::filesystem::exists(path)) {
            return DEFAULT;
        }
        std::ifstream in(path, std::ios::in | std::ios::binary);

        if (!in.is_open()) {
            return DEFAULT;
        }
        float logZoom = 0;
        uint64_t maxIteration = 0;
        std::string real;
        std::string imag;
        if (!readExact(in, logZoom) || !readExact(in, maxIteration) || !std::isfinite(logZoom) ||
            maxIteration == 0 || !readText(in, real) || !readText(in, imag)) {
            return DEFAULT;
        }

        return RFFLocationBinary(logZoom, std::move(real), std::move(imag), maxIteration);
    }


    bool RFFLocationBinary::hasData() const {
        return !real.empty() && !imag.empty();
    }


    void RFFLocationBinary::exportAsKeyframe(const std::filesystem::path &dir) const {
        exportFile(IOUtilities::generateFilename(dir, Constants::File::EXT_LOCATION, nullptr));
    }


    void RFFLocationBinary::exportFile(const std::filesystem::path &path) const {
        if (std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc); out.is_open()) {
            uint64_t len = 0;
            IOUtilities::encodeAndWrite(out, getLogZoom());
            IOUtilities::encodeAndWrite(out, maxIteration);
            len = real.length();
            IOUtilities::encodeAndWrite(out, len);
            IOUtilities::encodeAndWrite(out, real.data(), real.length());
            len = imag.length();
            IOUtilities::encodeAndWrite(out, len);
            IOUtilities::encodeAndWrite(out, imag.data(), imag.length());
            out.close();
        } else {
            vkh::logger::log_err("ERROR : Cannot save file");
        }
    }


    const std::string &RFFLocationBinary::getReal() const {
        return real;
    }

    const std::string &RFFLocationBinary::getImag() const {
        return imag;
    }

    uint64_t RFFLocationBinary::getMaxIteration() const {
        return maxIteration;
    }
}
