#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace merutilm::rff2 {
    class RFF2;

    class RenderPool final {
        struct Impl;
        std::unique_ptr<Impl> impl;

    public:
        RenderPool();
        ~RenderPool();
        RenderPool(const RenderPool &) = delete;
        RenderPool &operator=(const RenderPool &) = delete;

        void update(RFF2 &app);
        void renderPanel(RFF2 &app);
        void shutdown(RFF2 *app = nullptr);

        [[nodiscard]] bool isActive() const;
        [[nodiscard]] bool ownsNavigation() const;
    };
} // namespace merutilm::rff2
