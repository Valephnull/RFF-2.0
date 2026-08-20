#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace merutilm::rff2 {

    enum class RenderPoolMessageType : uint16_t {
        SERVER_HELLO = 1,
        AUTHENTICATE = 2,
        AUTHENTICATION_RESULT = 3,
        JOB = 10,
        REQUEST_TASK = 11,
        TASK = 12,
        NO_TASK = 13,
        RESULT = 14,
        WORKER_STATE = 15,
        JOB_STATE = 16,
        LEAVE = 17,
        FRAME_STATES = 18
    };

    enum class RenderPoolNetworkEventType : uint8_t {
        LISTENING,
        DISCOVERING,
        PASSWORD_REQUIRED,
        AUTHENTICATED,
        PEER_AUTHENTICATED,
        MESSAGE,
        DISCONNECTED,
        FAILURE
    };

    struct RenderPoolNetworkEvent {
        RenderPoolNetworkEventType type = RenderPoolNetworkEventType::FAILURE;
        uint64_t peerId = 0;
        std::string peerName;
        std::string text;
        RenderPoolMessageType messageType = RenderPoolMessageType::LEAVE;
        std::vector<std::byte> payload;
    };

    class RenderPoolNetwork final {
        struct Impl;
        std::unique_ptr<Impl> impl;

    public:
        static constexpr uint16_t PORT = 48191;
        static constexpr uint16_t DISCOVERY_PORT = 48192;

        explicit RenderPoolNetwork(uint16_t port = PORT, uint16_t discoveryPort = DISCOVERY_PORT);
        ~RenderPoolNetwork();
        RenderPoolNetwork(const RenderPoolNetwork &) = delete;
        RenderPoolNetwork &operator=(const RenderPoolNetwork &) = delete;

        bool startHost(std::string password, bool advertiseOnLan = false);
        bool join(std::string address, std::string workerName);
        bool joinLan(std::string workerName);
        void submitPassword(std::string password);
        void stop();

        [[nodiscard]] bool isRunning() const;
        [[nodiscard]] bool isHost() const;
        [[nodiscard]] std::vector<RenderPoolNetworkEvent> takeEvents();

        bool sendToServer(RenderPoolMessageType type, std::span<const std::byte> payload = {});
        bool sendToPeer(uint64_t peerId, RenderPoolMessageType type, std::span<const std::byte> payload = {});
        void broadcast(RenderPoolMessageType type, std::span<const std::byte> payload = {});

        [[nodiscard]] static std::string localIPv4();
    };
} // namespace merutilm::rff2
