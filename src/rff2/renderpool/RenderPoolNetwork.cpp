#include "RenderPoolNetwork.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "RenderPoolBinary.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace merutilm::rff2 {
    namespace {
#ifdef _WIN32
        using SocketHandle = SOCKET;
        constexpr SocketHandle INVALID_HANDLE = INVALID_SOCKET;
#else
        using SocketHandle = int;
        constexpr SocketHandle INVALID_HANDLE = -1;
#endif
        constexpr std::array<std::byte, 4> MESSAGE_MAGIC = {
                std::byte{'R'}, std::byte{'F'}, std::byte{'P'}, std::byte{'1'}};
        constexpr uint16_t PROTOCOL_VERSION = 1;
        constexpr uint64_t MAX_MESSAGE_SIZE = 1ULL << 30;
        constexpr uint64_t MAX_HANDSHAKE_SIZE = 4096;
        constexpr int PASSWORD_DERIVATION_ITERATIONS = 120000;
        constexpr std::string_view DISCOVERY_REQUEST = "RFF-EXP-DISCOVER-1";
        constexpr std::string_view DISCOVERY_RESPONSE = "RFF-EXP-HERE-1";

        template<typename Function>
        class ScopeExit final {
            Function function;

        public:
            explicit ScopeExit(Function &&value) : function(std::forward<Function>(value)) {}
            ~ScopeExit() { function(); }
            ScopeExit(const ScopeExit &) = delete;
            ScopeExit &operator=(const ScopeExit &) = delete;
        };

        void closeSocket(const SocketHandle socket) {
            if (socket == INVALID_HANDLE)
                return;
#ifdef _WIN32
            shutdown(socket, SD_BOTH);
            closesocket(socket);
#else
            shutdown(socket, SHUT_RDWR);
            close(socket);
#endif
        }

        bool sendAll(const SocketHandle socket, const std::byte *data, size_t length) {
            while (length > 0) {
                const auto chunk = static_cast<int>(std::min<size_t>(length, 1U << 20));
#ifdef _WIN32
                const int sent = send(socket, reinterpret_cast<const char *>(data), chunk, 0);
#else
                const int sent = static_cast<int>(send(socket, data, static_cast<size_t>(chunk), MSG_NOSIGNAL));
#endif
                if (sent <= 0)
                    return false;
                data += sent;
                length -= static_cast<size_t>(sent);
            }
            return true;
        }

        bool receiveAll(const SocketHandle socket, std::byte *data, size_t length) {
            while (length > 0) {
                const auto chunk = static_cast<int>(std::min<size_t>(length, 1U << 20));
#ifdef _WIN32
                const int received = recv(socket, reinterpret_cast<char *>(data), chunk, 0);
#else
                const int received = static_cast<int>(recv(socket, data, static_cast<size_t>(chunk), 0));
#endif
                if (received <= 0)
                    return false;
                data += received;
                length -= static_cast<size_t>(received);
            }
            return true;
        }

        bool sendMessage(const SocketHandle socket, const RenderPoolMessageType type,
                         const std::span<const std::byte> payload) {
            RenderPoolBinaryWriter header;
            header.bytes(MESSAGE_MAGIC);
            header.integer(PROTOCOL_VERSION);
            header.integer(static_cast<uint16_t>(type));
            header.integer(static_cast<uint64_t>(payload.size()));
            return sendAll(socket, header.view().data(), header.view().size()) &&
                   (payload.empty() || sendAll(socket, payload.data(), payload.size()));
        }

        bool receiveMessage(const SocketHandle socket, RenderPoolMessageType &type, std::vector<std::byte> &payload,
                            const uint64_t maximumSize = MAX_MESSAGE_SIZE) {
            std::array<std::byte, 16> headerBytes{};
            if (!receiveAll(socket, headerBytes.data(), headerBytes.size()))
                return false;
            if (!std::equal(MESSAGE_MAGIC.begin(), MESSAGE_MAGIC.end(), headerBytes.begin()))
                return false;
            RenderPoolBinaryReader reader(std::span(headerBytes).subspan(MESSAGE_MAGIC.size()));
            uint16_t version = 0;
            uint16_t rawType = 0;
            uint64_t length = 0;
            if (!reader.integer(version) || !reader.integer(rawType) || !reader.integer(length) || !reader.finished() ||
                version != PROTOCOL_VERSION || length > maximumSize)
                return false;
            type = static_cast<RenderPoolMessageType>(rawType);
            payload.resize(static_cast<size_t>(length));
            return payload.empty() || receiveAll(socket, payload.data(), payload.size());
        }

        std::array<std::byte, 32> derivePasswordProof(const std::string &password,
                                                      const std::span<const std::byte> nonce) {
            std::array<std::byte, 32> result{};
            if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                                  reinterpret_cast<const unsigned char *>(nonce.data()),
                                  static_cast<int>(nonce.size()), PASSWORD_DERIVATION_ITERATIONS, EVP_sha256(),
                                  static_cast<int>(result.size()),
                                  reinterpret_cast<unsigned char *>(result.data())) != 1) {
                result.fill(std::byte{0});
            }
            return result;
        }

        std::string socketAddressString(const sockaddr_in &address) {
            std::array<char, INET_ADDRSTRLEN> buffer{};
            if (inet_ntop(AF_INET, &address.sin_addr, buffer.data(), static_cast<socklen_t>(buffer.size())) == nullptr)
                return {};
            return buffer.data();
        }
    }

    struct RenderPoolNetwork::Impl {
        struct Peer {
            uint64_t id = 0;
            std::atomic<SocketHandle> socket = INVALID_HANDLE;
            std::mutex sendMutex;
            std::string name;
            std::atomic<bool> authenticated = false;

            bool send(const RenderPoolMessageType type, const std::span<const std::byte> payload) {
                std::scoped_lock lock(sendMutex);
                return sendMessage(socket.load(), type, payload);
            }
        };

        std::atomic<bool> running = false;
        std::atomic<bool> host = false;
        std::atomic<SocketHandle> listenerSocket = INVALID_HANDLE;
        std::atomic<SocketHandle> serverSocket = INVALID_HANDLE;
        std::atomic<SocketHandle> discoverySocket = INVALID_HANDLE;
        std::jthread listenerThread;
        std::jthread discoveryThread;
        std::jthread clientThread;
        std::jthread clientSenderThread;
        std::mutex peerMutex;
        std::unordered_map<uint64_t, std::shared_ptr<Peer>> peers;
        std::vector<std::jthread> peerThreads;
        std::atomic<uint64_t> nextPeerId = 1;
        std::mutex eventMutex;
        std::deque<RenderPoolNetworkEvent> events;
        std::string hostPassword;
        std::mutex passwordMutex;
        std::condition_variable passwordCondition;
        std::string submittedPassword;
        bool passwordSubmitted = false;
        std::mutex clientSendMutex;
        std::condition_variable clientSendCondition;
        std::deque<std::pair<RenderPoolMessageType, std::vector<std::byte>>> clientSendQueue;
        uint16_t port;
        uint16_t discoveryPort;
#ifdef _WIN32
        bool winsockStarted = false;
#endif

        Impl(const uint16_t portValue, const uint16_t discoveryPortValue)
            : port(portValue), discoveryPort(discoveryPortValue) {
#ifdef _WIN32
            WSADATA data{};
            winsockStarted = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
        }

        ~Impl() {
            stop();
#ifdef _WIN32
            if (winsockStarted)
                WSACleanup();
#endif
        }

        void push(RenderPoolNetworkEvent event) {
            std::scoped_lock lock(eventMutex);
            events.emplace_back(std::move(event));
        }

        void stop() {
            running.exchange(false);
            passwordCondition.notify_all();
            clientSendCondition.notify_all();
            const SocketHandle listener = listenerSocket.exchange(INVALID_HANDLE);
            const SocketHandle server = serverSocket.exchange(INVALID_HANDLE);
            const SocketHandle discovery = discoverySocket.exchange(INVALID_HANDLE);
            closeSocket(listener);
            closeSocket(server);
            closeSocket(discovery);
            {
                std::scoped_lock lock(peerMutex);
                for (const auto &[id, peer]: peers)
                    closeSocket(peer->socket.exchange(INVALID_HANDLE));
            }
            if (listenerThread.joinable()) {
                listenerThread.request_stop();
                listenerThread.join();
            }
            if (discoveryThread.joinable()) {
                discoveryThread.request_stop();
                discoveryThread.join();
            }
            if (clientThread.joinable()) {
                clientThread.request_stop();
                clientThread.join();
            }
            if (clientSenderThread.joinable()) {
                clientSenderThread.request_stop();
                clientSendCondition.notify_all();
                clientSenderThread.join();
            }
            {
                std::scoped_lock lock(clientSendMutex);
                clientSendQueue.clear();
            }
            for (auto &thread: peerThreads)
                thread.request_stop();
            peerThreads.clear();
            {
                std::scoped_lock lock(peerMutex);
                peers.clear();
            }
            host = false;
        }

        void runDiscoveryResponder() {
            const SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket == INVALID_HANDLE)
                return;
            discoverySocket = socket;
#ifdef _WIN32
            int exclusive = 1;
            setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                       sizeof(exclusive));
#else
            int reuse = 1;
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#endif
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(discoveryPort);
            if (bind(socket, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0) {
                closeSocket(discoverySocket.exchange(INVALID_HANDLE));
                push({.type = RenderPoolNetworkEventType::FAILURE,
                      .text = "LAN discovery could not listen; another LAN pool may already be running"});
                return;
            }
            ip_mreq membership{};
            inet_pton(AF_INET, "239.255.77.70", &membership.imr_multiaddr);
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&membership),
                       sizeof(membership));

            std::array<char, 64> request{};
            while (running) {
                sockaddr_in remote{};
#ifdef _WIN32
                int remoteLength = sizeof(remote);
#else
                socklen_t remoteLength = sizeof(remote);
#endif
                const int received = recvfrom(socket, request.data(), static_cast<int>(request.size()), 0,
                                              reinterpret_cast<sockaddr *>(&remote), &remoteLength);
                if (received <= 0)
                    break;
                if (std::string_view(request.data(), static_cast<size_t>(received)) != DISCOVERY_REQUEST)
                    continue;
                sendto(socket, DISCOVERY_RESPONSE.data(), static_cast<int>(DISCOVERY_RESPONSE.size()), 0,
                       reinterpret_cast<const sockaddr *>(&remote), remoteLength);
            }
            if (discoverySocket.exchange(INVALID_HANDLE) == socket)
                closeSocket(socket);
        }

        std::string discoverLanHost() {
            const SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket == INVALID_HANDLE)
                return {};
            discoverySocket = socket;
            int broadcast = 1;
            setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&broadcast),
                       sizeof(broadcast));
#ifdef _WIN32
            DWORD timeout = 3000;
#else
            timeval timeout{.tv_sec = 3, .tv_usec = 0};
#endif
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));

            const auto sendRequest = [this, socket](const uint32_t address) {
                sockaddr_in destination{};
                destination.sin_family = AF_INET;
                destination.sin_addr.s_addr = address;
                destination.sin_port = htons(discoveryPort);
                sendto(socket, DISCOVERY_REQUEST.data(), static_cast<int>(DISCOVERY_REQUEST.size()), 0,
                       reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
            };
            sendRequest(htonl(INADDR_BROADCAST));
            sendRequest(htonl(INADDR_LOOPBACK));
            in_addr multicast{};
            if (inet_pton(AF_INET, "239.255.77.70", &multicast) == 1)
                sendRequest(multicast.s_addr);

            std::array<char, 64> response{};
            sockaddr_in remote{};
#ifdef _WIN32
            int remoteLength = sizeof(remote);
#else
            socklen_t remoteLength = sizeof(remote);
#endif
            const int received = recvfrom(socket, response.data(), static_cast<int>(response.size()), 0,
                                          reinterpret_cast<sockaddr *>(&remote), &remoteLength);
            if (discoverySocket.exchange(INVALID_HANDLE) == socket)
                closeSocket(socket);
            if (received <= 0 ||
                std::string_view(response.data(), static_cast<size_t>(received)) != DISCOVERY_RESPONSE)
                return {};
            return socketAddressString(remote);
        }

        void runPeer(const std::shared_ptr<Peer> &peer) {
            ScopeExit cleanup([this, peer] {
                const bool authenticated = peer->authenticated.exchange(false);
                closeSocket(peer->socket.exchange(INVALID_HANDLE));
                {
                    std::scoped_lock lock(peerMutex);
                    peers.erase(peer->id);
                }
                if (authenticated) {
                    push({.type = RenderPoolNetworkEventType::DISCONNECTED, .peerId = peer->id,
                          .peerName = peer->name, .text = "Worker disconnected"});
                }
            });
            std::array<std::byte, 24> nonce{};
            if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), static_cast<int>(nonce.size())) != 1) {
                push({.type = RenderPoolNetworkEventType::FAILURE, .peerId = peer->id,
                      .text = "Could not generate a render-pool authentication challenge"});
                return;
            }
            RenderPoolBinaryWriter hello;
            hello.boolean(!hostPassword.empty());
            hello.bytes(nonce);
            if (!peer->send(RenderPoolMessageType::SERVER_HELLO, hello.view()))
                return;

            RenderPoolMessageType type{};
            std::vector<std::byte> payload;
            if (!receiveMessage(peer->socket.load(), type, payload, MAX_HANDSHAKE_SIZE) ||
                type != RenderPoolMessageType::AUTHENTICATE)
                return;
            RenderPoolBinaryReader auth(payload);
            std::string name;
            uint32_t proofLength = 0;
            if (!auth.string(name, 128) || !auth.integer(proofLength) || proofLength > 32 ||
                auth.remaining() != proofLength) {
                return;
            }
            std::vector<std::byte> proof;
            if (!auth.bytes(proof, proofLength) || !auth.finished())
                return;

            bool accepted = hostPassword.empty();
            if (!hostPassword.empty() && proof.size() == 32) {
                const auto expected = derivePasswordProof(hostPassword, nonce);
                accepted = CRYPTO_memcmp(expected.data(), proof.data(), expected.size()) == 0;
            }
            RenderPoolBinaryWriter response;
            response.boolean(accepted);
            response.string(accepted ? "Connected" : "Incorrect password");
            if (!peer->send(RenderPoolMessageType::AUTHENTICATION_RESULT, response.view()) || !accepted)
                return;

            peer->name = name.empty() ? "Worker" : name;
            peer->authenticated = true;
            push({.type = RenderPoolNetworkEventType::PEER_AUTHENTICATED, .peerId = peer->id,
                  .peerName = peer->name, .text = "Worker connected"});

            while (running) {
                payload.clear();
                if (!receiveMessage(peer->socket.load(), type, payload))
                    break;
                push({.type = RenderPoolNetworkEventType::MESSAGE, .peerId = peer->id,
                      .peerName = peer->name, .messageType = type, .payload = std::move(payload)});
            }
        }

        void runListener() {
            const SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_HANDLE) {
                push({.type = RenderPoolNetworkEventType::FAILURE, .text = "Could not create the render-pool server"});
                running = false;
                return;
            }
            listenerSocket = socket;
#ifdef _WIN32
            int exclusive = 1;
            setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                       sizeof(exclusive));
#else
            int reuse = 1;
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#endif
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(port);
            if (bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
                listen(socket, 16) != 0) {
                push({.type = RenderPoolNetworkEventType::FAILURE,
                      .text = "Could not listen on the render-pool port; another pool may already be running"});
                closeSocket(listenerSocket.exchange(INVALID_HANDLE));
                running = false;
                return;
            }
            push({.type = RenderPoolNetworkEventType::LISTENING, .text = "Render pool is accepting workers"});
            while (running) {
                sockaddr_in remote{};
#ifdef _WIN32
                int remoteLength = sizeof(remote);
#else
                socklen_t remoteLength = sizeof(remote);
#endif
                const SocketHandle accepted = accept(socket, reinterpret_cast<sockaddr *>(&remote), &remoteLength);
                if (accepted == INVALID_HANDLE)
                    break;
                auto peer = std::make_shared<Peer>();
                peer->id = nextPeerId++;
                peer->socket = accepted;
                {
                    std::scoped_lock lock(peerMutex);
                    peers.emplace(peer->id, peer);
                    peerThreads.emplace_back([this, peer] { runPeer(peer); });
                }
            }
        }

        void runClient(const std::string address, const std::string workerName) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            addrinfo *addresses = nullptr;
            const std::string portText = std::to_string(port);
            if (getaddrinfo(address.c_str(), portText.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
                push({.type = RenderPoolNetworkEventType::FAILURE, .text = "The render-pool address is invalid"});
                running = false;
                return;
            }
            SocketHandle socket = INVALID_HANDLE;
            for (auto *current = addresses; current != nullptr; current = current->ai_next) {
                socket = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
                if (socket != INVALID_HANDLE &&
                    connect(socket, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0)
                    break;
                closeSocket(socket);
                socket = INVALID_HANDLE;
            }
            freeaddrinfo(addresses);
            if (socket == INVALID_HANDLE) {
                push({.type = RenderPoolNetworkEventType::FAILURE,
                      .text = "Could not connect to that render pool"});
                running = false;
                return;
            }
            serverSocket = socket;
            ScopeExit cleanup([this] {
                closeSocket(serverSocket.exchange(INVALID_HANDLE));
            });

            RenderPoolMessageType type{};
            std::vector<std::byte> payload;
            if (!receiveMessage(socket, type, payload, MAX_HANDSHAKE_SIZE) ||
                type != RenderPoolMessageType::SERVER_HELLO) {
                push({.type = RenderPoolNetworkEventType::FAILURE,
                      .text = "The server did not speak the RFF-EXP render-pool protocol"});
                running = false;
                return;
            }
            RenderPoolBinaryReader hello(payload);
            bool requiresPassword = false;
            std::vector<std::byte> nonce;
            if (!hello.boolean(requiresPassword) || !hello.bytes(nonce, 24) || !hello.finished()) {
                push({.type = RenderPoolNetworkEventType::FAILURE, .text = "Invalid render-pool greeting"});
                running = false;
                return;
            }

            std::string password;
            if (requiresPassword) {
                push({.type = RenderPoolNetworkEventType::PASSWORD_REQUIRED,
                      .text = "This render pool requires a password"});
                std::unique_lock lock(passwordMutex);
                passwordCondition.wait(lock, [this] { return passwordSubmitted || !running; });
                if (!running)
                    return;
                password = std::move(submittedPassword);
                submittedPassword.clear();
                passwordSubmitted = false;
            }

            RenderPoolBinaryWriter authentication;
            authentication.string(workerName);
            if (requiresPassword) {
                const auto proof = derivePasswordProof(password, nonce);
                authentication.integer<uint32_t>(static_cast<uint32_t>(proof.size()));
                authentication.bytes(proof);
            } else {
                authentication.integer<uint32_t>(0);
            }
            if (!sendMessage(socket, RenderPoolMessageType::AUTHENTICATE, authentication.view()) ||
                !receiveMessage(socket, type, payload, MAX_HANDSHAKE_SIZE) ||
                type != RenderPoolMessageType::AUTHENTICATION_RESULT) {
                push({.type = RenderPoolNetworkEventType::FAILURE, .text = "Render-pool authentication failed"});
                running = false;
                return;
            }
            RenderPoolBinaryReader result(payload);
            bool accepted = false;
            std::string text;
            if (!result.boolean(accepted) || !result.string(text, 256) || !result.finished() || !accepted) {
                push({.type = RenderPoolNetworkEventType::FAILURE,
                      .text = text.empty() ? "Render-pool authentication was rejected" : text});
                running = false;
                return;
            }
            push({.type = RenderPoolNetworkEventType::AUTHENTICATED, .text = text});

            while (running) {
                payload.clear();
                if (!receiveMessage(socket, type, payload))
                    break;
                push({.type = RenderPoolNetworkEventType::MESSAGE, .messageType = type,
                      .payload = std::move(payload)});
            }
            if (running)
                push({.type = RenderPoolNetworkEventType::DISCONNECTED, .text = "Disconnected from render pool"});
            running = false;
            clientSendCondition.notify_all();
        }

        void runClientSender(const std::stop_token &stopToken) {
            while (!stopToken.stop_requested()) {
                std::pair<RenderPoolMessageType, std::vector<std::byte>> message;
                {
                    std::unique_lock lock(clientSendMutex);
                    clientSendCondition.wait(lock, [this, &stopToken] {
                        return stopToken.stop_requested() || !running || !clientSendQueue.empty();
                    });
                    if (stopToken.stop_requested() || !running)
                        return;
                    message = std::move(clientSendQueue.front());
                    clientSendQueue.pop_front();
                }
                const SocketHandle socket = serverSocket.load();
                if (socket == INVALID_HANDLE || !sendMessage(socket, message.first, message.second)) {
                    push({.type = RenderPoolNetworkEventType::FAILURE,
                          .text = "Could not send data to the render-pool host"});
                    running = false;
                    closeSocket(serverSocket.exchange(INVALID_HANDLE));
                    clientSendCondition.notify_all();
                    return;
                }
            }
        }
    };

    RenderPoolNetwork::RenderPoolNetwork(const uint16_t port, const uint16_t discoveryPort)
        : impl(std::make_unique<Impl>(port, discoveryPort)) {}

    RenderPoolNetwork::~RenderPoolNetwork() = default;

    bool RenderPoolNetwork::startHost(std::string password, const bool advertiseOnLan) {
        stop();
        impl->hostPassword = std::move(password);
        impl->host = true;
        impl->running = true;
        impl->listenerThread = std::jthread([this] { impl->runListener(); });
        if (advertiseOnLan)
            impl->discoveryThread = std::jthread([this] { impl->runDiscoveryResponder(); });
        return true;
    }

    bool RenderPoolNetwork::join(std::string address, std::string workerName) {
        stop();
        {
            std::scoped_lock lock(impl->passwordMutex);
            impl->submittedPassword.clear();
            impl->passwordSubmitted = false;
        }
        impl->host = false;
        impl->running = true;
        impl->clientSenderThread = std::jthread([this](const std::stop_token &stopToken) {
            impl->runClientSender(stopToken);
        });
        impl->clientThread = std::jthread(
                [this, address = std::move(address), workerName = std::move(workerName)] {
                    impl->runClient(address, workerName);
                });
        return true;
    }

    bool RenderPoolNetwork::joinLan(std::string workerName) {
        stop();
        {
            std::scoped_lock lock(impl->passwordMutex);
            impl->submittedPassword.clear();
            impl->passwordSubmitted = false;
        }
        impl->host = false;
        impl->running = true;
        impl->clientSenderThread = std::jthread([this](const std::stop_token &stopToken) {
            impl->runClientSender(stopToken);
        });
        impl->clientThread = std::jthread([this, workerName = std::move(workerName)] {
            impl->push({.type = RenderPoolNetworkEventType::DISCOVERING,
                        .text = "Searching for a render pool on this LAN"});
            const std::string address = impl->discoverLanHost();
            if (address.empty()) {
                if (impl->running)
                    impl->push({.type = RenderPoolNetworkEventType::FAILURE,
                                .text = "No LAN render pool answered the discovery request"});
                impl->running = false;
                impl->clientSendCondition.notify_all();
                return;
            }
            impl->runClient(address, workerName);
        });
        return true;
    }

    void RenderPoolNetwork::submitPassword(std::string password) {
        {
            std::scoped_lock lock(impl->passwordMutex);
            impl->submittedPassword = std::move(password);
            impl->passwordSubmitted = true;
        }
        impl->passwordCondition.notify_all();
    }

    void RenderPoolNetwork::stop() { impl->stop(); }

    bool RenderPoolNetwork::isRunning() const { return impl->running; }

    bool RenderPoolNetwork::isHost() const { return impl->host; }

    std::vector<RenderPoolNetworkEvent> RenderPoolNetwork::takeEvents() {
        std::vector<RenderPoolNetworkEvent> result;
        std::scoped_lock lock(impl->eventMutex);
        result.reserve(impl->events.size());
        while (!impl->events.empty()) {
            result.emplace_back(std::move(impl->events.front()));
            impl->events.pop_front();
        }
        return result;
    }

    bool RenderPoolNetwork::sendToServer(const RenderPoolMessageType type,
                                         const std::span<const std::byte> payload) {
        if (!impl->running || impl->host || impl->serverSocket.load() == INVALID_HANDLE)
            return false;
        {
            std::scoped_lock lock(impl->clientSendMutex);
            if (!impl->running)
                return false;
            impl->clientSendQueue.emplace_back(type, std::vector<std::byte>(payload.begin(), payload.end()));
        }
        impl->clientSendCondition.notify_one();
        return true;
    }

    bool RenderPoolNetwork::sendToPeer(const uint64_t peerId, const RenderPoolMessageType type,
                                       const std::span<const std::byte> payload) {
        std::shared_ptr<Impl::Peer> peer;
        {
            std::scoped_lock lock(impl->peerMutex);
            const auto it = impl->peers.find(peerId);
            if (it == impl->peers.end() || !it->second->authenticated)
                return false;
            peer = it->second;
        }
        return peer->send(type, payload);
    }

    void RenderPoolNetwork::broadcast(const RenderPoolMessageType type, const std::span<const std::byte> payload) {
        std::vector<std::shared_ptr<Impl::Peer>> peers;
        {
            std::scoped_lock lock(impl->peerMutex);
            for (const auto &[id, peer]: impl->peers) {
                if (peer->authenticated)
                    peers.push_back(peer);
            }
        }
        for (const auto &peer: peers)
            peer->send(type, payload);
    }

    std::string RenderPoolNetwork::localIPv4() {
        const SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket != INVALID_HANDLE) {
            sockaddr_in route{};
            route.sin_family = AF_INET;
            route.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &route.sin_addr);
            if (connect(socket, reinterpret_cast<const sockaddr *>(&route), sizeof(route)) == 0) {
                sockaddr_in local{};
#ifdef _WIN32
                int length = sizeof(local);
#else
                socklen_t length = sizeof(local);
#endif
                if (getsockname(socket, reinterpret_cast<sockaddr *>(&local), &length) == 0) {
                    closeSocket(socket);
                    return socketAddressString(local);
                }
            }
            closeSocket(socket);
        }

        std::array<char, 256> hostName{};
        if (gethostname(hostName.data(), static_cast<int>(hostName.size())) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *addresses = nullptr;
            if (getaddrinfo(hostName.data(), nullptr, &hints, &addresses) == 0) {
                std::string fallback;
                for (auto *current = addresses; current != nullptr; current = current->ai_next) {
                    const auto *candidate = reinterpret_cast<const sockaddr_in *>(current->ai_addr);
                    const std::string address = socketAddressString(*candidate);
                    if (!address.empty() && address != "127.0.0.1") {
                        fallback = address;
                        break;
                    }
                }
                freeaddrinfo(addresses);
                if (!fallback.empty())
                    return fallback;
            }
        }
        return "127.0.0.1";
    }
}
