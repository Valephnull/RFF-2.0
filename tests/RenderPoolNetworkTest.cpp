#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <thread>

#include "rff2/renderpool/RenderPoolNetwork.hpp"

using namespace merutilm::rff2;

namespace {
    bool waitFor(RenderPoolNetwork &network, const std::function<bool(const RenderPoolNetworkEvent &)> &predicate,
                 RenderPoolNetworkEvent *matched = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto &event: network.takeEvents()) {
                if (event.type == RenderPoolNetworkEventType::FAILURE) {
                    std::cerr << event.text << '\n';
                    return false;
                }
                if (predicate(event)) {
                    if (matched != nullptr)
                        *matched = std::move(event);
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
}

int main() {
    constexpr uint16_t TEST_PORT = 48291;
    constexpr uint16_t TEST_DISCOVERY_PORT = 48292;
    RenderPoolNetwork host(TEST_PORT, TEST_DISCOVERY_PORT);
    RenderPoolNetwork client(TEST_PORT, TEST_DISCOVERY_PORT);
    if (!host.startHost("test password") ||
        !waitFor(host, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::LISTENING;
        })) {
        std::cerr << "Host did not begin listening\n";
        return 1;
    }

    if (!client.join("127.0.0.1", "Test Worker") ||
        !waitFor(client, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::PASSWORD_REQUIRED;
        })) {
        std::cerr << "Client was not challenged for a password\n";
        return 2;
    }
    client.submitPassword("test password");

    RenderPoolNetworkEvent hostConnection;
    if (!waitFor(client, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::AUTHENTICATED;
        }) ||
        !waitFor(host, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::PEER_AUTHENTICATED;
        }, &hostConnection)) {
        std::cerr << "Password authentication did not complete\n";
        return 3;
    }

    const std::array<std::byte, 4> payload = {
            std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    if (!host.sendToPeer(hostConnection.peerId, RenderPoolMessageType::JOB, payload)) {
        std::cerr << "Host could not send a job message\n";
        return 4;
    }
    RenderPoolNetworkEvent received;
    if (!waitFor(client, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::MESSAGE &&
                   event.messageType == RenderPoolMessageType::JOB;
        }, &received) || !std::equal(payload.begin(), payload.end(), received.payload.begin(), received.payload.end())) {
        std::cerr << "Client did not receive the job payload\n";
        return 5;
    }

    if (!client.sendToServer(RenderPoolMessageType::REQUEST_TASK) ||
        !waitFor(host, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::MESSAGE &&
                   event.messageType == RenderPoolMessageType::REQUEST_TASK;
        })) {
        std::cerr << "Host did not receive the task request\n";
        return 6;
    }

    const std::array<std::byte, 3> progressPayload = {
            std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    if (!client.sendToServer(RenderPoolMessageType::WORKER_STATE, progressPayload) ||
        !waitFor(host, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::MESSAGE &&
                   event.messageType == RenderPoolMessageType::WORKER_STATE;
        }, &received) ||
        !std::equal(progressPayload.begin(), progressPayload.end(), received.payload.begin(), received.payload.end())) {
        std::cerr << "Host did not receive the worker progress state\n";
        return 7;
    }

    if (!host.sendToPeer(hostConnection.peerId, RenderPoolMessageType::FRAME_STATES, progressPayload) ||
        !waitFor(client, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::MESSAGE &&
                   event.messageType == RenderPoolMessageType::FRAME_STATES;
        }, &received) ||
        !std::equal(progressPayload.begin(), progressPayload.end(), received.payload.begin(), received.payload.end())) {
        std::cerr << "Worker did not receive the frame progress state\n";
        return 8;
    }

    client.stop();
    host.stop();

    if (!host.startHost("", true) ||
        !waitFor(host, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::LISTENING;
        })) {
        std::cerr << "LAN host did not begin listening\n";
        return 9;
    }
    if (!client.joinLan("Discovered Worker") ||
        !waitFor(client, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::AUTHENTICATED;
        }) ||
        !waitFor(host, [](const RenderPoolNetworkEvent &event) {
            return event.type == RenderPoolNetworkEventType::PEER_AUTHENTICATED;
        })) {
        std::cerr << "LAN discovery did not connect without credentials\n";
        return 10;
    }
    client.stop();
    host.stop();
    return 0;
}
