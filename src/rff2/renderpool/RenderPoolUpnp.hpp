#pragma once

#include <cstdint>
#include <string>

namespace merutilm::rff2 {

    struct RenderPoolUpnpMapping {
        std::string controlUrl;
        std::string serviceType;
        std::string internalAddress;
        std::string externalAddress;
        uint16_t port = 0;
        bool mapped = false;
    };

    struct RenderPoolUpnpResult {
        RenderPoolUpnpMapping mapping;
        std::string message;

        [[nodiscard]] bool succeeded() const { return mapping.mapped && !mapping.externalAddress.empty(); }
    };

    class RenderPoolUpnp final {
    public:
        static RenderPoolUpnpResult open(uint16_t port);
        static void close(const RenderPoolUpnpMapping &mapping);
    };
} // namespace merutilm::rff2
