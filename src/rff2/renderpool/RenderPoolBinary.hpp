#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace merutilm::rff2 {

    class RenderPoolBinaryWriter {
        std::vector<std::byte> data;

    public:
        template<typename T> requires std::is_integral_v<T>
        void integer(T value) {
            using U = std::make_unsigned_t<T>;
            U unsignedValue = static_cast<U>(value);
            for (size_t i = 0; i < sizeof(T); ++i) {
                const size_t shift = (sizeof(T) - i - 1) * 8;
                data.push_back(static_cast<std::byte>((unsignedValue >> shift) & 0xffU));
            }
        }

        void boolean(const bool value) { integer<uint8_t>(value ? 1 : 0); }

        void floating(const float value) { integer(std::bit_cast<uint32_t>(value)); }

        void bytes(const std::span<const std::byte> value) {
            data.insert(data.end(), value.begin(), value.end());
        }

        void string(const std::string_view value) {
            integer<uint32_t>(static_cast<uint32_t>(value.size()));
            bytes(std::as_bytes(std::span(value.data(), value.size())));
        }

        [[nodiscard]] const std::vector<std::byte> &view() const { return data; }

        [[nodiscard]] std::vector<std::byte> take() { return std::move(data); }
    };

    class RenderPoolBinaryReader {
        std::span<const std::byte> data;
        size_t offset = 0;
        bool valid = true;

    public:
        explicit RenderPoolBinaryReader(const std::span<const std::byte> source) : data(source) {}

        template<typename T> requires std::is_integral_v<T>
        bool integer(T &value) {
            if (!valid || offset > data.size() || data.size() - offset < sizeof(T)) {
                valid = false;
                return false;
            }
            using U = std::make_unsigned_t<T>;
            U result = 0;
            for (size_t i = 0; i < sizeof(T); ++i) {
                result = static_cast<U>((result << 8) | std::to_integer<uint8_t>(data[offset++]));
            }
            value = static_cast<T>(result);
            return true;
        }

        bool boolean(bool &value) {
            uint8_t raw = 0;
            if (!integer(raw) || raw > 1) {
                valid = false;
                return false;
            }
            value = raw != 0;
            return true;
        }

        bool floating(float &value) {
            uint32_t bits = 0;
            if (!integer(bits))
                return false;
            value = std::bit_cast<float>(bits);
            return true;
        }

        bool string(std::string &value, const uint32_t maximumLength = 1U << 20) {
            uint32_t length = 0;
            if (!integer(length) || length > maximumLength || offset > data.size() || data.size() - offset < length) {
                valid = false;
                return false;
            }
            value.assign(reinterpret_cast<const char *>(data.data() + offset), length);
            offset += length;
            return true;
        }

        bool bytes(std::vector<std::byte> &value, const size_t length) {
            if (!valid || offset > data.size() || data.size() - offset < length) {
                valid = false;
                return false;
            }
            value.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                         data.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
            return true;
        }

        [[nodiscard]] bool finished() const { return valid && offset == data.size(); }

        [[nodiscard]] bool good() const { return valid; }

        [[nodiscard]] size_t remaining() const { return offset <= data.size() ? data.size() - offset : 0; }
    };
}
