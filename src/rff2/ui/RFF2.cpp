//
// Created by Merutilm on 2025-08-08.
//

#include "RFF2.hpp"

#include "../calc/dex_exp.h"
#include "../mb/MB2Locator.h"
#include "../mb/MandelbrotFeatureFinder.hpp"
#include "../parallel/ParallelArrayDispatcher.h"
#include "../parallel/ParallelDispatcher.h"
#include "../preset/calc/CalculationPresets.h"
#include "../preset/render/RenderPresets.h"
#include "../preset/shader/bloom/ShdBloomPresets.h"
#include "../preset/shader/color/ShdColorPresets.h"
#include "../preset/shader/fog/ShdFogPresets.h"
#include "../preset/shader/palette/ShdPalettePresets.h"
#include "../preset/shader/slope/ShdSlopePresets.h"
#include "../preset/shader/stripe/ShdStripePresets.h"
#include "../vulkan/GPCDownsampleForBlur.hpp"
#include "../vulkan/SharedDescriptorTemplate.hpp"
#include "../vulkan/SharedImageContextIndices.hpp"
#include "FnExplore.hpp"
#include "FnFile.hpp"
#include "FnFractal.hpp"
#include "FnPreset.hpp"
#include "FnRender.hpp"
#include "FnShader.hpp"
#include "FnVideo.hpp"
#include "IOUtilities.h"
#include "Utilities.h"
#include "imgui.h"
#include "nfd.hpp"
#include "opencv2/opencv.hpp"
#include "vulkan_helper/engine/executor/ScopedNewCommandBufferExecutor.hpp"
#include "vulkan_helper/engine/window/PlatformWindow.hpp"
#include "vulkan_helper/util/BarrierUtils.hpp"
#include "vulkan_helper/util/BufferImageContextUtils.hpp"


namespace merutilm::rff2 {


    void RFF2::onStart() {
        cursorManager = std::make_unique<CursorManager>(rootWindowContext->getWindow()->getWindow());

        applyShaderSettings(settings);
        refreshResizeParams(rootWindowContext->getSwapchain().getSwapchainExtent());
        requests.requestRecompute();
        initImGui();
        NFD::Init();
    }

    void RFF2::onResize(const VkExtent2D newExtent) {
        Application::onResize(newExtent);
        if (newExtent.width > 0 || newExtent.height > 0) {
            engine->getCore().getLogicalDevice().waitDeviceIdle();
            state.cancel();
            refreshResizeParams(newExtent);
            requests.requestRecompute();
            backgroundThreads.notifyAll();
        }
    }

    void RFF2::onQuit() {
        autoExplorer.stop();
        state.cancel();
        renderer = nullptr;
        NFD::Quit();
    }

    void RFF2::update() {

        // Mouse-wheel input changes the smooth preview immediately, but CPU renders
        // are coalesced. Never cancel and join a render for every wheel tick.
        constexpr double WHEEL_RENDER_DEBOUNCE_SECONDS = 0.09;
        const double now = rootWindowContext->getWindow()->getTime();
        if (wheelZoomRenderPending && idleCompute.load() &&
            now - wheelZoomLastInputTime >= WHEEL_RENDER_DEBOUNCE_SECONDS) {
            wheelZoomRenderPending = false;
            requests.requestRecompute();
        }

        if (requests.defaultSettingsRequested) {
            applyDefaultSettings();
            requests.defaultSettingsRequested.exchange(false);
            backgroundThreads.notifyAll();
        }

        if (requests.shaderRequested) {
            applyShaderSettings(settings);
            requests.shaderRequested.exchange(false);
            backgroundThreads.notifyAll();
        }

        if (requests.resizeRequested) {
            onResize(requests.resizeRequestedExtent);
            requests.resizeRequested.exchange(false);
            backgroundThreads.notifyAll();
        }

        if (requests.recomputeRequested) {
            canShowPreview = false;
            idleCompute = false;
            requests.recomputeRequested.exchange(false);
            recomputeThreaded();
            // it is threaded, not idle
        }

        if (requests.createImageRequested) {
            applyCreateImage();
            requests.createImageRequested.exchange(false);
            backgroundThreads.notifyAll();
        }

        autoExplorer.update(*this);

        invokeUpdaters();
        renderer->render();
    }


    Settings RFF2::genDefaultSettings() {
#ifndef NDEBUG
        return Settings{
                .fractal =
                        FractalSettings{.general = {.bailout = 2.00001f, .logZoom = 2, .threads = 1},
                                        .reference =
                                                {
                                                        .center = fixed_point_complex_i1(
                                                                "-0.85", "0", Perturbator::logZoomToExp10(2)),
                                                        .useParallelRefCalculation = false,
                                                        .sync = CalculationPresets::UltraBest().genRefSync(),
                                                        .compression = CalculationPresets::UltraStable().genRefComp(),
                                                        .reuse = false,
                                                },
                                        .sa = {.use = false,
                                               .appliedTermsCount = 8,
                                               .validatedTermsCount = 1,
                                               .epsilonPower = -5},
                                        .mpa = CalculationPresets::Stable().genMPA(),
                                        .perturb = {.maxIteration = 300,
                                                    .decimalizeIterationMethod = FrtDecimalizeIterationMethod::LOG_LOG,
                                                    .autoMaxIteration = true,
                                                    .interiorDetectRadiusPower = 7,
                                                    .autoIterationMultiplier = 100,
                                                    .absoluteIterationMode = false}},
                .render = {.clarityMultiplier = 0.25f, .fps = 60, .linearInterpolation = true},
                .shader = {.palette = ShdPalettePresets::LongRandom64().genPalette(),
                           .stripe = ShdStripePresets::Disabled().genStripe(),
                           .slope = ShdSlopePresets::Disabled().genSlope(),
                           .color = ShdColorPresets::Disabled().genColor(),
                           .fog = ShdFogPresets::Disabled().genFog(),
                           .bloom = BloomPresets::Disabled().genBloom()},
                .video = {.data = {.defaultZoomIncrement = 2, .isStatic = false},
                          .animation = {.overZoom = 2, .showText = true, .mps = 1},
                          .exportation = {.fps = 60, .bitrate = 9000}}};
#else
        return Settings{
                .fractal =
                        FractalSettings{.general = {.bailout = 2.00001f,
                                                    .logZoom = 2,
                                                    .threads = std::thread::hardware_concurrency() - 1},
                                        .reference =
                                                {
                                                        .center = fixed_point_complex_i1(
                                                                "-0.85", "0", Perturbator::logZoomToExp10(2)),
                                                        .useParallelRefCalculation = false,
                                                        .sync = CalculationPresets::UltraFast().genRefSync(),
                                                        .compression = CalculationPresets::UltraFast().genRefComp(),
                                                        .reuse = false,
                                                },
                                        .sa = {.use = false,
                                               .appliedTermsCount = 8,
                                               .validatedTermsCount = 1,
                                               .epsilonPower = -5},
                                        .mpa = CalculationPresets::UltraFast().genMPA(),
                                        .perturb = {.maxIteration = 300,
                                                    .decimalizeIterationMethod = FrtDecimalizeIterationMethod::LOG_LOG,
                                                    .autoMaxIteration = true,
                                                    .interiorDetectRadiusPower = 7,
                                                    .autoIterationMultiplier = 100,
                                                    .absoluteIterationMode = false}},
                .render = RenderPresets::High().genRender(),
                .shader = {.palette = ShdPalettePresets::LongRandom64().genPalette(),
                           .stripe = ShdStripePresets::Disabled().genStripe(),
                           .slope = ShdSlopePresets::Disabled().genSlope(),
                           .color = ShdColorPresets::Disabled().genColor(),
                           .fog = ShdFogPresets::Disabled().genFog(),
                           .bloom = BloomPresets::Disabled().genBloom()},
                .video = {.data = {.defaultZoomIncrement = 2, .isStatic = false},
                          .animation = {.overZoom = 2, .showText = true, .mps = 1},
                          .exportation = {.fps = 60, .bitrate = 9000}}};
#endif
    }

    complex<dex> RFF2::offsetConversion(const Settings &s, const int mx, const int my) const {
        using namespace Constants::Fractal;
        const double ox = static_cast<double>(mx) - static_cast<double>(getIterationBufferWidth()) / 2.0;
        const double oy = static_cast<double>(my) - static_cast<double>(getIterationBufferHeight()) / 2.0;

        return {dex(std::abs(ox) < INTENTIONAL_ERROR_OFFSET_MIN_PIX ? INTENTIONAL_ERROR_OFFSET_MIN_PIX : ox) /
                        getDivisor(s) / dex(s.render.clarityMultiplier),
                dex(std::abs(oy) < INTENTIONAL_ERROR_OFFSET_MIN_PIX ? INTENTIONAL_ERROR_OFFSET_MIN_PIX : oy) /
                        getDivisor(s) / dex(s.render.clarityMultiplier)};
    }

    dex RFF2::getDivisor(const Settings &settings) { return dex_exp::exp10(settings.fractal.general.logZoom); }


    uint16_t RFF2::calcIterationBufferWidth(const Settings &s) const {
        const float multiplier = s.render.clarityMultiplier;
        return static_cast<uint16_t>(static_cast<float>(rootWindowContext->getSwapchain().getSwapchainExtent().width) *
                                     multiplier);
    }

    uint16_t RFF2::calcIterationBufferHeight(const Settings &s) const {
        const float multiplier = s.render.clarityMultiplier;
        return static_cast<uint16_t>(static_cast<float>(rootWindowContext->getSwapchain().getSwapchainExtent().height) *
                                     multiplier);
    }

    uint16_t RFF2::getIterationBufferWidth() const { return renderer->rg0->iterationPalette->iterWidth; }

    uint16_t RFF2::getIterationBufferHeight() const { return renderer->rg0->iterationPalette->iterHeight; }

    RFF2::GuidedZoomTarget RFF2::findGuidedZoomTarget(const int mouseX, const int mouseY) const {
        const MB2RenderDataBase *data = renderData.get();
        const MB2PerturbatorBase *perturbator = data ? data->getPerturbator() : nullptr;
        if (!guidedZoom || !idleCompute.load() || !data || !data->getReference() || !perturbator)
            return {};

        const int width = getIterationBufferWidth();
        const int height = getIterationBufferHeight();
        const int cursorX = std::clamp(mouseX, 0, width - 1);
        const int cursorY = std::clamp(mouseY, 0, height - 1);
        const float clarity = std::max(settings.render.clarityMultiplier, 0.001f);
        const dex divisor = getDivisor(settings);
        const complex<dex> cursorOffsetFromCurrent = {
                dex(static_cast<double>(cursorX) - static_cast<double>(width) / 2.0) / divisor / dex(clarity),
                dex(static_cast<double>(cursorY) - static_cast<double>(height) / 2.0) / divisor / dex(clarity),
        };
        const complex<dex> cursorOffsetFromReference = cursorOffsetFromCurrent + perturbator->off;

        // Imagina searches at two cursor-centred radii: 1/24 and 1/12 in its
        // normalized coordinates, equivalent to 1/48 and 1/24 of the view height.
        const int maximumRadius = std::max(4, std::min(width, height) / 24);
        const int minimumRadius = std::max(2, maximumRadius / 2);
        const std::array radii = {minimumRadius, maximumRadius};
        GuidedZoomTarget selected = {};
        double selectedDistance = 0;

        for (const int radiusPixels: radii) {
            const dex radius = dex(static_cast<double>(radiusPixels)) / divisor / dex(clarity);
            const std::optional feature = MandelbrotFeatureFinder::find(*data, cursorOffsetFromReference, radius);
            if (!feature)
                continue;

            const complex<dex> offsetFromCurrent = feature->offsetFromReference - perturbator->off;
            const double featureX = static_cast<double>(offsetFromCurrent.re * divisor * dex(clarity)) +
                                    static_cast<double>(width) / 2.0;
            const double featureY = static_cast<double>(offsetFromCurrent.im * divisor * dex(clarity)) +
                                    static_cast<double>(height) / 2.0;
            const double distance =
                    std::hypot(featureX - static_cast<double>(cursorX), featureY - static_cast<double>(cursorY));
            if (!selected.found || feature->estimatedSize > selected.estimatedSize ||
                (feature->estimatedSize == selected.estimatedSize && distance < selectedDistance)) {
                selected = {
                        static_cast<float>(featureX),
                        static_cast<float>(featureY),
                        feature->preperiod,
                        feature->period,
                        feature->estimatedSize,
                        feature->kind == MandelbrotFeatureFinder::Kind::MISIUREWICZ,
                        true,
                };
                selectedDistance = distance;
            }
        }
        return selected;
    }

    void RFF2::refreshGuidedZoomTarget(const int mouseX, const int mouseY) {
        if (!guidedZoom || !mouseInsideWindow || isNavigationLocked() || ImGui::GetIO().WantCaptureMouse) {
            guidedZoomTarget = {};
            guidedZoomTargetCached = false;
            return;
        }
        if (!idleCompute.load())
            return;

        constexpr int CURSOR_QUANTIZATION = 4;
        constexpr double MINIMUM_SEARCH_INTERVAL = 0.075;
        const int quantizedX = mouseX / CURSOR_QUANTIZATION * CURSOR_QUANTIZATION;
        const int quantizedY = mouseY / CURSOR_QUANTIZATION * CURSOR_QUANTIZATION;
        const uint64_t render = completedRenderCount.load();
        if (guidedZoomTargetCached && guidedZoomMouseX == quantizedX && guidedZoomMouseY == quantizedY &&
            guidedZoomTargetRender == render) {
            return;
        }

        const double now = rootWindowContext->getWindow()->getTime();
        if (guidedZoomTargetCached && guidedZoomTargetRender == render &&
            now - guidedZoomLastSearchTime < MINIMUM_SEARCH_INTERVAL) {
            return;
        }

        guidedZoomMouseX = static_cast<int16_t>(quantizedX);
        guidedZoomMouseY = static_cast<int16_t>(quantizedY);
        guidedZoomTargetRender = render;
        guidedZoomLastSearchTime = now;
        guidedZoomTarget = findGuidedZoomTarget(quantizedX, quantizedY);
        guidedZoomTargetCached = true;
    }

    void RFF2::renderGuidedZoomOverlay() {
        if (!guidedZoom || !mouseInsideWindow || isNavigationLocked() || ImGui::GetIO().WantCaptureMouse) {
            guidedZoomTarget = {};
            guidedZoomTargetCached = false;
            return;
        }

        double mouseWindowX;
        double mouseWindowY;
        glfwGetCursorPos(rootWindowContext->getWindow()->getWindow(), &mouseWindowX, &mouseWindowY);
        refreshGuidedZoomTarget(getMouseXOnIterationBuffer(static_cast<int>(mouseWindowX)),
                                getMouseYOnIterationBuffer(static_cast<int>(mouseWindowY)));
        if (!guidedZoomTarget.found)
            return;

        const float clarity = std::max(settings.render.clarityMultiplier, 0.001f);
        const ImVec2 cursor = {static_cast<float>(mouseWindowX), static_cast<float>(mouseWindowY)};
        const ImVec2 feature = {
                guidedZoomTarget.x / clarity,
                static_cast<float>(getIterationBufferHeight() - guidedZoomTarget.y) / clarity,
        };
        const ImU32 color = IM_COL32(255, 255, 255, 230);
        const std::string label = guidedZoomTarget.misiurewicz
                                          ? std::format("M {}+{}", guidedZoomTarget.preperiod, guidedZoomTarget.period)
                                          : std::format("P {}", guidedZoomTarget.period);
        ImDrawList *drawList = ImGui::GetForegroundDrawList();
        drawList->AddLine(cursor, feature, color, 2.0f);
        drawList->AddText({feature.x + 8.0f, feature.y - 8.0f}, color, label.c_str());
    }


    void RFF2::addListeners() {
        Application::addListeners();
        auto &eventSystem = rootWindowContext->getWindow()->eventSystem;

        eventSystem.mouse.onMouseEnter.add([this] {
            mouseInsideWindow = true;
            glfwSetCursor(cursorManager->window, cursorManager->crosshairCursor);
        });
        eventSystem.mouse.onMouseExit.add([this] {
            mouseInsideWindow = false;
            guidedZoomTarget = {};
            guidedZoomTargetCached = false;
            glfwSetCursor(cursorManager->window, nullptr);
        });


        eventSystem.mouse.onMouseMove.add([this](const int mx, const int my) {
            const uint16_t x = getMouseXOnIterationBuffer(mx);
            const uint16_t y = getMouseYOnIterationBuffer(my);
            if (renderer->iterationStagingBufferContext == nullptr) {
                return;
            }
            auto it = static_cast<uint64_t>((*renderer->iterationStagingBufferContext)(x, y));
            setStatusMessage(Constants::Status::ITERATION_STATUS,
                             std::format(std::locale("en_US.UTF-8"), "Iterations : {:L}", it, x, y));
        });

        eventSystem.mouseDrag.onMouseDrag.add(
                [this](const int mb, const int mx, const int my, const int mdx, const int mdy) {
                    if (isNavigationLocked())
                        return;
                    const int16_t x = getMouseXOnIterationBuffer(mx);
                    const int16_t y = getMouseYOnIterationBuffer(my);
                    const auto dx = static_cast<int16_t>(getMouseXOnIterationBuffer(mx - mdx) - x);
                    const auto dy = static_cast<int16_t>(getMouseYOnIterationBuffer(my - mdy) - y);
                    const auto dxr = -static_cast<float>(dx) / static_cast<float>(getIterationBufferWidth());
                    const auto dyr = static_cast<float>(dy) / static_cast<float>(getIterationBufferHeight());
                    const auto dz = pow(10.0f, -zoomAnimationInfo.targetLogZoomOffsetAim);

                    zoomAnimationInfo.aimChanged = true;
                    zoomAnimationInfo.targetMouseDragOffset += glm::vec2{dxr * dz, dyr * dz};

                    if (mb == GLFW_MOUSE_BUTTON_LEFT) {
                        const float m = settings.render.clarityMultiplier;
                        const float logZoom = settings.fractal.general.logZoom;
                        const int exp10 = Perturbator::logZoomToExp10(logZoom);

                        fixed_point_complex_i1 &center = settings.fractal.reference.center;
                        center.set_exp10(exp10);
                        const fixed_point_complex_i1 add(dex(static_cast<float>(dx) / m) / getDivisor(settings),
                                                         dex(static_cast<float>(dy) / m) / getDivisor(settings), exp10);
                        fixed_point_complex_i1::add(center, center, add);

                        requests.requestRecompute();
                    }
                });
        eventSystem.mouseWheel.onMouseScroll.add([this](const int value) {
            if (isNavigationLocked())
                return;
            settings.fractal.general.logZoom = std::max(Constants::Fractal::ZOOM_MIN, settings.fractal.general.logZoom);
            double mdx;
            double mdy;
            glfwGetCursorPos(rootWindowContext->getWindow()->getWindow(), &mdx, &mdy);
            const int mx = static_cast<int>(mdx);
            const int my = static_cast<int>(mdy);
            const int16_t mix = getMouseXOnIterationBuffer(mx);
            const int16_t miy = getMouseYOnIterationBuffer(my);
            const float increment = value > 0 ? Constants::Fractal::ZOOM_INTERVAL : -Constants::Fractal::ZOOM_INTERVAL;
            int16_t anchorX = mix;
            int16_t anchorY = miy;
            bool featureRedirected = false;

            if (guidedZoom) {
                refreshGuidedZoomTarget(mix, miy);
                if (value > 0 && guidedZoomTarget.found) {
                    const double scale = std::pow(10.0, static_cast<double>(increment));
                    const double inverseScaleDifference = 1.0 / (scale - 1.0);
                    const double redirectedAnchorX =
                            (scale * static_cast<double>(guidedZoomTarget.x) - static_cast<double>(mix)) *
                            inverseScaleDifference;
                    const double redirectedAnchorY =
                            (scale * static_cast<double>(guidedZoomTarget.y) - static_cast<double>(miy)) *
                            inverseScaleDifference;

                    // An off-centre zoom can move a feature to the stationary cursor
                    // exactly. If that requires an anchor outside the view, leave this
                    // wheel step cursor-centred instead of making a wild jump.
                    if (redirectedAnchorX >= 0.0 && redirectedAnchorX < getIterationBufferWidth() &&
                        redirectedAnchorY >= 0.0 && redirectedAnchorY < getIterationBufferHeight()) {
                        anchorX = static_cast<int16_t>(std::lround(redirectedAnchorX));
                        anchorY = static_cast<int16_t>(std::lround(redirectedAnchorY));
                        featureRedirected = true;
                    }
                }
            }

            zoom(anchorX, anchorY, increment, false);

            if (featureRedirected) {
                const float scale = std::pow(10.0f, increment);
                guidedZoomTarget.x = scale * guidedZoomTarget.x + (1.0f - scale) * anchorX;
                guidedZoomTarget.y = scale * guidedZoomTarget.y + (1.0f - scale) * anchorY;
            }
        });
    }


    void RFF2::zoom(const int16_t px, const int16_t py, const float logIncrement, const bool requestRender) {

        settings.fractal.general.logZoom = std::max(Constants::Fractal::ZOOM_MIN, settings.fractal.general.logZoom);
        const int16_t mix = px;
        const int16_t miy = py;
        const auto mxr = static_cast<float>(mix) / static_cast<float>(getIterationBufferWidth()) - 0.5f;
        const auto myr = static_cast<float>(miy) / static_cast<float>(getIterationBufferHeight()) - 0.5f;
        const auto dz = pow(10.0f, -zoomAnimationInfo.targetLogZoomOffsetAim);

        const auto [re, im] = offsetConversion(settings, mix, miy);
        float &logZoom = settings.fractal.general.logZoom;
        fixed_point_complex_i1 &center = settings.fractal.reference.center;
        const int exp10 = Perturbator::logZoomToExp10(logZoom);
        center.set_exp10(exp10);

        const float mz = pow(10.0f, -logIncrement);
        logZoom += logIncrement;
        const fixed_point_complex_i1 add(re * dex(1 - mz), im * dex(1 - mz), exp10);
        fixed_point_complex_i1::add(center, center, add);

        zoomAnimationInfo.aimChanged = true;
        zoomAnimationInfo.stop();
        zoomAnimationInfo.targetLogZoomOffsetAim += logIncrement;
        zoomAnimationInfo.targetMouseZoomOffsetAim += glm::vec2{mxr * dz * (mz - 1), myr * dz * (1 - mz)};
        if (requestRender) {
            requests.requestRecompute();
        } else {
            wheelZoomRenderPending = true;
            wheelZoomLastInputTime = rootWindowContext->getWindow()->getTime();
        }
    }


    void RFF2::applyDefaultSettings() {
        rootWindowContext->core.getLogicalDevice().waitDeviceIdle();
        settings = genDefaultSettings();
    }


    void RFF2::applyCreateImage() {
        const uint32_t frameIndex = renderer->getFrameIndex();
        rootWindowContext->getSyncObject().getFence(frameIndex).wait();

        if (requests.createImageRequestedFilename.empty()) {
            const auto path = IOUtilities::ioFileDialog(Constants::File::DESC_IMAGE, IOUtilities::SAVE_FILE,
                                                        Constants::File::EXT_IMAGE);

            if (path == nullptr)
                return;

            requests.createImageRequestedFilename = path->string();
        }
        const auto &imgCtx = rootWindowContext->getSharedImageContext().getImageContextMF(
                SharedImageContextIndices::MF_MAIN_RENDER_IMAGE_SECONDARY)[frameIndex];

        vkh::BufferContext bufCtx = vkh::BufferContext::createContext(
                rootWindowContext->core,
                {
                        .size = imgCtx.capacity,
                        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        .properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                });
        vkh::BufferContext::mapMemory(rootWindowContext->core, bufCtx);
        // NEW COMMAND BUFFER
        {
            const auto executor =
                    vkh::ScopedNewCommandBufferExecutor(rootWindowContext->core, rootWindowContext->getCommandPool());
            vkh::BarrierUtils::cmdImageMemoryBarrier(
                    executor.getCommandBufferHandle(), imgCtx.image, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkh::BufferImageContextUtils::cmdCopyImageToBuffer(executor.getCommandBufferHandle(), imgCtx, bufCtx);
        }
        vkh::BufferContext::unmapMemory(rootWindowContext->core, bufCtx);

        auto img = cv::Mat(static_cast<int>(imgCtx.extent.height), static_cast<int>(imgCtx.extent.width), CV_16UC4,
                           bufCtx.mappedMemory);
        cv::cvtColor(img, img, cv::COLOR_RGBA2BGRA);
        cv::imwrite(requests.createImageRequestedFilename, img);
        vkh::BufferContext::destroyContext(rootWindowContext->core, bufCtx);
    }

    void RFF2::invokeUpdaters() {
        static float time = rootWindowContext->getWindow()->getTime();
        const float t = rootWindowContext->getWindow()->getTime();
        const float dt = t - time;
        time = t;

        if (canShowPreview && !wheelZoomRenderPending && !zoomAnimationInfo.aimChanged) {
            renderer->iterationStagingBufferContext->fill();
            renderer->rg0->iterationPalette->applyMaxIteration();
            zoomAnimationInfo.reset();
        }

        zoomAnimationInfo.update(dt);
        renderer->rccPresentPrepare->smoothZoom->setSmoothZoomData(zoomAnimationInfo.targetMouseDragOffset +
                                                                           zoomAnimationInfo.targetMouseZoomOffset,
                                                                   zoomAnimationInfo.targetLogZoomOffset);
    }

    void RFF2::applyShaderSettings(const Settings &s) const {
        rootWindowContext->core.getLogicalDevice().waitDeviceIdle();
        renderer->rg0->iterationPalette->setPalette(s.shader.palette);
        renderer->rg0->stripe->setStripe(s.shader.stripe);
        renderer->rg0->slope->setSlope(s.shader.slope);
        renderer->rg0->color->setColor(s.shader.color);
        renderer->rg3->fog->setFog(s.shader.fog);
        renderer->rg4->bloom->setBloom(s.shader.bloom);
        renderer->rg4->linearInterpolation->setLinearInterpolation(s.render.linearInterpolation);
        renderer->computeBoxBlur->setBlurInfo(CPCBoxBlur::DESC_INDEX_BLUR_TARGET_FOG, s.shader.fog.radius);
        renderer->computeBoxBlur->setBlurInfo(CPCBoxBlur::DESC_INDEX_BLUR_TARGET_BLOOM, s.shader.bloom.radius);
    }

    void RFF2::refreshResizeParams(const VkExtent2D swapchainExtent) {
        const uint16_t iw = calcIterationBufferWidth(settings);
        const uint16_t ih = calcIterationBufferHeight(settings);
        const auto &[dWidth, dHeight] =
                RendererUtils::getBlurredImageExtent(swapchainExtent, settings.render.clarityMultiplier);
        const auto &[sWidth, sHeight] = rootWindowContext->getSwapchain().getSwapchainExtent();

        renderer->rccDownsample->downsample->setRescaledResolution(GPCDownsampleForBlur::DESC_INDEX_RESAMPLE_IMAGE_FOG,
                                                                   {dWidth, dHeight});
        renderer->rccDownsample->downsample->setRescaledResolution(
                GPCDownsampleForBlur::DESC_INDEX_RESAMPLE_IMAGE_BLOOM, {dWidth, dHeight});

        renderer->rccPresentPrepare->smoothZoom->setRescaledResolution({sWidth, sHeight});
        renderer->rg0->iterationPalette->resetIterationBuffer(iw, ih);
        iterationMatrix = std::make_unique<Matrix<double>>(iw, ih);
        renderer->iterationStagingBufferContext = std::make_unique<GraphicsMatrixBuffer<double>>(
                rootWindowContext->core, iw, ih, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }

    void RFF2::registerRenderers() {
        renderer = registerRenderer<AppRenderer>(*engine, *rootWindowContext, settings, [this] { renderImGui(); });
        createImGuiContext(renderer->imguiRenderContext);
    }

    void RFF2::initImGui() {

        const ImGuiIO &io = ImGui::GetIO();
        const std::filesystem::path path =
                vkh::ExecutableUtils::getExecutableDirectory() / ".." / "res" / "IBMPlexSansKR-Medium.ttf";
        io.Fonts->AddFontFromFileTTF(path.string().data(), 20.0f, nullptr, io.Fonts->GetGlyphRangesKorean());

        ImGuiStyle &style = ImGui::GetStyle();

        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, .8f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.07f, 0.08f, .8f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.18f, .9f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.28f, .8f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.24f, 0.24f, 0.24f, .9f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.34f, 0.34f, 0.34f, 1.0f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.32f, 0.56f, 1.0f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.50f, 0.95f, 1.0f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.34f, 0.60f, 1.00f, 1.0f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.24f, 0.50f, 0.95f, 1.0f);

        style.Colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.50f, 0.95f, 1.0f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.50f, 0.95f, 0.85f);
        style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
        style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);

        style.WindowRounding = 8.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 6.0f;
        style.PopupRounding = 8.0f;
        style.TabRounding = 6.0f;
        style.ScrollbarRounding = 8.0f;

        style.FrameBorderSize = 0.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
    }


    void RFF2::renderImGui() {

        renderControlImGui();
        renderStatusImGui();
        renderGuidedZoomOverlay();
    }

    void RFF2::renderControlImGui() {
        ImGui::Begin("Control");
        const bool controlsLocked = isNavigationLocked();
        if (ImGui::BeginTabBar("Control")) {
            if (ImGui::BeginTabItem("File")) {
                ImGui::BeginDisabled(controlsLocked);
                FnFile::saveMap(*this);
                FnFile::saveImage(*this);
                FnFile::saveLocation(*this);
                FnFile::loadMap(*this);
                FnFile::loadLocation(*this);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Fractal")) {
                ImGui::BeginDisabled(controlsLocked);
                FnFractal::reference(*this);
                FnFractal::iterations(*this);
                // FnFractal::sa(*this);
                FnFractal::mpa(*this);
                FnFractal::automaticIterations(*this);
                FnFractal::absoluteIterationMode(*this);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Render")) {
                ImGui::BeginDisabled(controlsLocked);
                FnRender::setResolutionProperties(*this);
                FnRender::setRenderProperties(*this);
                FnRender::linearInterpolation(*this);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Presets")) {
                ImGui::BeginDisabled(controlsLocked);
                FnPreset::calculation(*this);
                FnPreset::render(*this);
                FnPreset::resolution(*this);
                FnPreset::shader(*this);

                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Shader")) {
                ImGui::BeginDisabled(controlsLocked);
                FnShader::palette(*this);
                FnShader::stripe(*this);
                FnShader::slope(*this);
                FnShader::color(*this);
                FnShader::fog(*this);
                FnShader::bloom(*this);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Video")) {
                ImGui::BeginDisabled(controlsLocked);
                FnVideo::dataSettings(*this);
                FnVideo::animationSettings(*this);
                FnVideo::exportSettings(*this);
                FnVideo::generateVidKeyframes(*this);
                FnVideo::exportZoomVideo(*this);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Explore")) {
                ImGui::BeginDisabled(controlsLocked);
                FnExplore::recompute(*this);
                FnExplore::reset(*this);
                FnExplore::cancelRender(*this);
                FnExplore::locateCenteredReference(*this);
                FnExplore::guidedZoom(*this);
                ImGui::EndDisabled();
                FnExplore::locateMinibrot(*this);
                ImGui::BeginDisabled(controlsLocked);
                FnExplore::autoExplorer(*this);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void RFF2::renderStatusImGui() const {

        const float height = ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y - height));

        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, height));

        ImGui::Begin("StatusBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
        if (ImGui::BeginTable("StatusBarTable", static_cast<int>(statusMessages.size()),
                              ImGuiTableFlags_BordersInner)) {
            for (const auto &statusMessage: statusMessages) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(statusMessage.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::PopStyleVar();
        ImGui::End();
    }

    void RFF2::refreshSharedImgContexts(const VkExtent2D extent) {
        using namespace SharedImageContextIndices;
        auto &sharedImg = rootWindowContext->getSharedImageContext();
        sharedImg.cleanupContexts();
        auto iiiGetter = [](const VkExtent2D ex, const VkFormat format, const VkImageUsageFlags usage) {
            return vkh::ImageInitInfo{
                    .imageType = VK_IMAGE_TYPE_2D,
                    .imageViewType = VK_IMAGE_VIEW_TYPE_2D,
                    .imageFormat = format,
                    .extent = {ex.width, ex.height, 1},
                    .useMipmap = VK_FALSE,
                    .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .imageTiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = usage,
                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            };
        };

        const auto internalImageExtent =
                RendererUtils::getInternalImageExtent(extent, settings.render.clarityMultiplier);
        const auto blurredImageExtent = RendererUtils::getBlurredImageExtent(extent, settings.render.clarityMultiplier);

        sharedImg.appendMultiframeImageContext(MF_MAIN_RENDER_IMAGE_PRIMARY,
                                               iiiGetter(internalImageExtent, VK_FORMAT_R16G16B16A16_UNORM,
                                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                                                 VK_IMAGE_USAGE_SAMPLED_BIT));
        sharedImg.appendMultiframeImageContext(
                MF_MAIN_RENDER_IMAGE_SECONDARY,
                iiiGetter(internalImageExtent, VK_FORMAT_R16G16B16A16_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        sharedImg.appendMultiframeImageContext(MF_MAIN_RENDER_DOWNSAMPLED_IMAGE_PRIMARY,
                                               iiiGetter(blurredImageExtent, VK_FORMAT_R8G8B8A8_UNORM,
                                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                 VK_IMAGE_USAGE_STORAGE_BIT));
        sharedImg.appendMultiframeImageContext(MF_MAIN_RENDER_DOWNSAMPLED_IMAGE_SECONDARY,
                                               iiiGetter(blurredImageExtent, VK_FORMAT_R8G8B8A8_UNORM,
                                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                 VK_IMAGE_USAGE_STORAGE_BIT));
    }

    void RFF2::overwriteMatrixFromMap(const RFFDynamicMapBinary &map) const {
        rootWindowContext->core.getLogicalDevice().waitDeviceIdle();
        const uint32_t iw = getIterationBufferWidth();
        const uint32_t ih = getIterationBufferHeight();
        if (iw != map.getMatrix().getWidth() || ih != map.getMatrix().getHeight()) {
            vkh::logger::log_err("Map size mismatch, {}x{} required but provided {}x{}", iw, ih,
                                 map.getMatrix().getWidth(), map.getMatrix().getHeight());
            return;
        }

        renderer->rg0->iterationPalette->setMaxIterationTemp(static_cast<double>(map.getMaxIteration()));
        renderer->iterationStagingBufferContext->fill(map.getMatrix().getCanvas());
    }

    int16_t RFF2::getMouseXOnIterationBuffer(const int mx) const {
        const float multiplier = settings.render.clarityMultiplier;
        return static_cast<int16_t>(static_cast<float>(mx) * multiplier);
    }

    int16_t RFF2::getMouseYOnIterationBuffer(const int my) const {
        const float multiplier = settings.render.clarityMultiplier;
        return static_cast<int16_t>(static_cast<float>(getIterationBufferHeight()) -
                                    static_cast<float>(my) * multiplier);
    }

    void RFF2::recomputeThreaded() {
        state.createThread([this] {
            const Settings s = this->settings; // clone the settings
            const auto start = rootWindowContext->getWindow()->getTime();
            bool success = false;

            try {
                success = prepareRenderData(start, s);

                if (success) {
                    beforeIterationFill();
                    success = fillIteration(start, s);
                }
            } catch (allocation_cancelled &) {
                vkh::logger::log("Memory allocation cancelled by user");
            }

            afterComputeFinally(success);
        });
    }

    void RFF2::beforeIterationFill() const {

        renderer->rg0->iterationPalette->setMaxIterationTemp(
                static_cast<double>(renderData->fractalSettings.perturb.maxIteration));
    }

    bool RFF2::prepareRenderData(const float startTime, const Settings &s) {


        canShowPreview = false;

        if (state.interruptRequested())
            return false;

        auto &frt = s.fractal;
        const float logZoom = frt.general.logZoom;

        setStatusMessage(Constants::Status::ZOOM_STATUS,
                         std::format("Zoom : {:.06f}E{:d}", pow(10, fmod(logZoom, 1)), static_cast<int>(logZoom)));

        const complex<dex> offset = offsetConversion(s, 0, 0);
        const dex dcMax = offset.norm_approx();

        static uint64_t capacity = 0;
        if (renderData && renderData->getReference()) {
            capacity = renderData->getReference()->length();
        }

        std::function actionPerRefCalcIteration = [this, startTime](const uint64_t p) mutable {
            static float time = rootWindowContext->getWindow()->getTime();
            const float elapsed = rootWindowContext->getWindow()->getTime() - time;
            if (elapsed > Constants::Status::UI_REFRESH_INTERVAL) {
                time = rootWindowContext->getWindow()->getTime();
                setStatusMessage(Constants::Status::RENDER_STATUS, std::format(std::locale(), "Period : {:L}", p));
                setStatusMessage(Constants::Status::TIME_STATUS,
                                 std::format("Time : {}", Utilities::formatTime(time - startTime)));
            }
        };
        std::function actionPerSeriesApproxIteration = [this, startTime](const uint64_t p, const double i) mutable {
            static float time = rootWindowContext->getWindow()->getTime();
            const float elapsed = rootWindowContext->getWindow()->getTime() - time;
            if (elapsed > Constants::Status::UI_REFRESH_INTERVAL) {
                time = rootWindowContext->getWindow()->getTime();
                setStatusMessage(Constants::Status::RENDER_STATUS,
                                 std::format("Series-Approximation : {:.3f}%", i * 100));
                setStatusMessage(Constants::Status::TIME_STATUS,
                                 std::format("Time : {}", Utilities::formatTime(time - startTime)));
            }
        };
        std::function actionPerCreatingTableIteration = [this, startTime](const uint64_t p, const double i) mutable {
            static float time = rootWindowContext->getWindow()->getTime();
            const float elapsed = rootWindowContext->getWindow()->getTime() - time;
            if (elapsed > Constants::Status::UI_REFRESH_INTERVAL) {
                time = rootWindowContext->getWindow()->getTime();
                setStatusMessage(Constants::Status::RENDER_STATUS, std::format("MP-Approximation : {:.3f}%", i * 100));
                setStatusMessage(Constants::Status::TIME_STATUS,
                                 std::format("Time : {}", Utilities::formatTime(time - startTime)));
            }
        };


        if (state.interruptRequested())
            return false;


        if (frt.reference.reuse) {
            if (!renderData || !renderData->getReference() || !renderData->getPerturbator()) {
                vkh::logger::log_err("Do not reuse Reference during reference calculation!!!");
                this->settings.fractal.reference.reuse = false;
                requests.requestRecompute();
                return false;
            }
            renderData->translate(frt.general.logZoom, renderData->getPerturbator()->dcMax, frt.perturb,
                                  frt.reference.center, actionPerSeriesApproxIteration);
        } else {
            const int exp10 = Perturbator::logZoomToExp10(logZoom);
            renderData = nullptr;
            if (logZoom > Constants::Fractal::ZOOM_DEADLINE) {
                renderData = std::make_unique<DeepMB2RenderData>(
                        state, frt, approxTableCache, dcMax, exp10, capacity, 0, actionPerRefCalcIteration,
                        actionPerSeriesApproxIteration, actionPerCreatingTableIteration, false);
            } else {
                renderData = std::make_unique<LightMB2RenderData>(
                        state, frt, approxTableCache, dcMax, exp10, capacity, 0, actionPerRefCalcIteration,
                        actionPerSeriesApproxIteration, actionPerCreatingTableIteration, false);
            }
        }

        const MB2ReferenceBase *reference = renderData->getReference();
        if (!reference || state.interruptRequested())
            return false;

        size_t refLength = reference->length();
        size_t mpaLen;
        if (const auto t = dynamic_cast<LightMB2RenderData *>(renderData.get())) {
            mpaLen = t->table->getLength();
        }
        if (const auto t = dynamic_cast<DeepMB2RenderData *>(renderData.get())) {
            mpaLen = t->table->getLength();
        }

        setStatusMessage(Constants::Status::PERIOD_STATUS,
                         std::format("Period : {:L} ({:L}, {:L})", reference->longestPeriod(), refLength, mpaLen));
        if (state.interruptRequested())
            return false;


        return true;
    }


    bool RFF2::fillIteration(const float startTime, const Settings &s) {

        if (state.interruptRequested())
            return false;

        canShowPreview = true;

        std::atomic renderPixelsCount = 0;
        const uint16_t w = getIterationBufferWidth();
        const uint16_t h = getIterationBufferHeight();
        uint32_t len = static_cast<uint32_t>(w) * h;

        auto rendered = std::vector<bool>(len);

        auto func = [&s, this, &renderPixelsCount, &rendered](const uint16_t x, const uint16_t y, const uint16_t xRes,
                                                              const uint16_t yRes, float, float, const uint32_t i,
                                                              double) {
            assert(i < rendered.size());
            rendered[i] = true;
            const auto dc = offsetConversion(s, x, y);
            const double iteration = renderData->getPerturbator()->iterate(dc);

            renderer->iterationStagingBufferContext->set(x, y, iteration);

            auto my = static_cast<int16_t>(y + 1);
            while (my < yRes && !rendered[my * xRes + x]) {
                renderer->iterationStagingBufferContext->set(x, my, iteration);
                ++my;
            }

            ++renderPixelsCount;
            return iteration;
        };
        auto previewer =
                ParallelArrayDispatcher<double>(state, *iterationMatrix, s.fractal.general.threads, std::move(func));

        // renderer->iterationStagingBufferContext->fillZero();

        auto statusThread = std::jthread([&renderPixelsCount, len, this, startTime](const std::stop_token &stop) {
            static float time = rootWindowContext->getWindow()->getTime();
            while (!stop.stop_requested()) {
                const float elapsed = rootWindowContext->getWindow()->getTime() - time;
                if (elapsed > Constants::Status::UI_REFRESH_INTERVAL) {
                    time = rootWindowContext->getWindow()->getTime();
                    float ratio = static_cast<float>(renderPixelsCount.load()) / static_cast<float>(len) * 100;
                    setStatusMessage(Constants::Status::TIME_STATUS,
                                     std::format("Time : {}", Utilities::formatTime(time - startTime)));
                    setStatusMessage(Constants::Status::RENDER_STATUS, std::format("Calculation : {:.3f}%", ratio));
                }
            }
        });


        previewer.dispatch();

        statusThread.request_stop();
        statusThread.join();

        if (state.interruptRequested())
            return false;

        const auto syncer = ParallelDispatcher(
                state, w, h, s.fractal.general.threads,
                [this](const uint16_t x, const uint16_t y, uint16_t, uint16_t, float, float, uint32_t) {
                    renderer->iterationStagingBufferContext->set(x, y, (*iterationMatrix)(x, y));
                });

        syncer.dispatch();

        if (state.interruptRequested())
            return false;

        setStatusMessage(Constants::Status::RENDER_STATUS, "Done");

        return true;
    }

    void RFF2::afterComputeFinally(const bool success) {
        if (!success) {
            // vkh::logger::log("Recompute cancelled.");
        }
        idleCompute = true;
        ++completedRenderCount;
        if (unlockNavigationAfterRender.exchange(false))
            navigationLocked = false;
        backgroundThreads.notifyAll();
    }
} // namespace merutilm::rff2
