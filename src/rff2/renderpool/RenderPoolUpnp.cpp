#include "RenderPoolUpnp.hpp"

#include <array>
#include <string>

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

namespace merutilm::rff2 {
    namespace {
        std::string commandError(const int code) {
            const char *text = strupnperror(code);
            return text == nullptr ? "unknown router error" : text;
        }
    }

    RenderPoolUpnpResult RenderPoolUpnp::open(const uint16_t port) {
        RenderPoolUpnpResult result;
        int discoveryError = 0;
        UPNPDev *devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &discoveryError);
        if (devices == nullptr) {
            result.message = "No UPnP internet gateway was found";
            return result;
        }

        UPNPUrls urls{};
        IGDdatas data{};
        std::array<char, 64> internalAddress{};
        std::array<char, 64> detectedWanAddress{};
        const int gateway = UPNP_GetValidIGD(devices, &urls, &data, internalAddress.data(),
                                             static_cast<int>(internalAddress.size()), detectedWanAddress.data(),
                                             static_cast<int>(detectedWanAddress.size()));
        freeUPNPDevlist(devices);
        if (gateway == 0 || urls.controlURL == nullptr || data.first.servicetype[0] == '\0') {
            FreeUPNPUrls(&urls);
            result.message = "No usable UPnP internet gateway was found";
            return result;
        }

        std::array<char, 64> externalAddress{};
        int addressResult = UPNPCOMMAND_SUCCESS;
        if (detectedWanAddress[0] != '\0') {
            externalAddress = detectedWanAddress;
        } else {
            addressResult = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype,
                                                      externalAddress.data());
        }
        const std::string portText = std::to_string(port);
        const int mappingResult = UPNP_AddPortMapping(
                urls.controlURL, data.first.servicetype, portText.c_str(), portText.c_str(), internalAddress.data(),
                "RFF-EXP Render Pool", "TCP", nullptr, "0");

        result.mapping.controlUrl = urls.controlURL;
        result.mapping.serviceType = data.first.servicetype;
        result.mapping.internalAddress = internalAddress.data();
        result.mapping.externalAddress = externalAddress.data();
        result.mapping.port = port;
        result.mapping.mapped = mappingResult == UPNPCOMMAND_SUCCESS;
        FreeUPNPUrls(&urls);

        if (mappingResult != UPNPCOMMAND_SUCCESS) {
            result.message = "UPnP could not map the render-pool port: " + commandError(mappingResult);
        } else if (addressResult != UPNPCOMMAND_SUCCESS || result.mapping.externalAddress.empty()) {
            result.message = "UPnP mapped the port but could not read the public address";
        } else {
            result.message = "UPnP opened TCP port " + portText;
        }
        return result;
    }

    void RenderPoolUpnp::close(const RenderPoolUpnpMapping &mapping) {
        if (!mapping.mapped || mapping.controlUrl.empty() || mapping.serviceType.empty() || mapping.port == 0)
            return;
        const std::string port = std::to_string(mapping.port);
        UPNP_DeletePortMapping(mapping.controlUrl.c_str(), mapping.serviceType.c_str(), port.c_str(), "TCP", nullptr);
    }
} // namespace merutilm::rff2
