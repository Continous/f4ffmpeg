#include "manager.h"

#include "config.h"
#include "pch.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

extern "C"
{
#include <libavutil/frame.h>
}

namespace f4ffmpeg
{
namespace
{
    transitionMethod configuredFallbackTransition()
    {
        static const transitionMethod method = []
        {
            const auto configured =
                config::fallbackTransitionMethod.GetValue();

            const auto parsed =
                parseTransitionMethod(
                    configured,
                    false
                );

            if (parsed)
                return *parsed;

            REX::WARN(
                "Unsupported global Transitions.FallbackTransitionMethod '{}'; using VanillaDDS. "
                "Global Image transitions are intentionally unsupported.",
                configured
            );

            return transitionMethod::vanillaDDS;
        }();

        return method;
    }
}

bool manager::start(
    const char* inputPath,
    videoPlaybackSettings settings)
{
    if (inputPath == nullptr)
    {
        return false;
    }

    if (running)
    {
        return false;
    }

    if (managerThread.joinable())
    {
        managerThread.join();
    }

    this->inputPath = inputPath;
    playbackSettings = std::move(settings);

    looping.store(
        playbackSettings.looping,
        std::memory_order_release
    );

    playbackSources.clear();
    playbackSources.emplace_back(this->inputPath);

    for (auto& entry : playbackSettings.playlist)
    {
        if (
            entry.empty() ||
            std::find(
                playbackSources.begin(),
                playbackSources.end(),
                entry
            ) != playbackSources.end())
        {
            continue;
        }

        playbackSources.emplace_back(entry);
    }

    currentSourceIndex = 0;
    shuffleEnabled = playbackSettings.shuffle;
    shuffleBag.clear();
    sourceGeneration = 1;
    transitionActive.store(
        false,
        std::memory_order_release
    );
    playlistTransitionFrame.store(
        nullptr,
        std::memory_order_release
    );

    if (
        shuffleEnabled &&
        playbackSources.size() > 1)
    {
        refillShuffleBag(true);
    }

    // Honor the global permission gate before even evaluating/preloading a
    // playlist-owned image. The global config never accepts an image path.
    if (
        config::usePlaylistTransitionMethod.GetValue() &&
        playbackSettings.transition == transitionMethod::image &&
        playbackSettings.transitionImage)
    {
        auto image =
            loadPlaylistTransitionImage(
                *playbackSettings.transitionImage
            );

        if (image)
        {
            playlistTransitionFrame.store(
                std::move(image),
                std::memory_order_release
            );
        }
        else
        {
            REX::WARN(
                "Playlist transition Method=Image could not preload '{}'; decoder transitions will use the global fallback method.",
                *playbackSettings.transitionImage
            );
        }
    }

    REX::INFO(
        "Manager playback policy: {} source(s), shuffle={}, loop={}, transition={}",
        playbackSources.size(),
        shuffleEnabled,
        looping.load(std::memory_order_acquire),
        transitionMethodName(resolveTransitionMethod())
    );

    if (!decoderWorker.start(inputPath))
    {
        return false;
    }

    if (!producerWorker.start())
    {
        decoderWorker.stop();
        return false;
    }

    lastSubmittedFrame = nullptr;

    stopRequested = false;
    running = true;

    managerThread = std::thread(
        &manager::run,
        this
    );

    return true;
}

std::shared_ptr<const producedFrame>
manager::getLatestFrame() const
{
    return producerWorker.getLatestFrame();
}

transitionPresentation
manager::getTransitionPresentation() const
{
    transitionPresentation presentation{};

    if (!transitionActive.load(
            std::memory_order_acquire))
    {
        return presentation;
    }

    presentation.active = true;
    presentation.method =
        activeTransitionMethod.load(
            std::memory_order_acquire
        );

    switch (presentation.method)
    {
        case transitionMethod::holdLastFrame:
            presentation.frame =
                producerWorker.getLatestFrame();
            break;

        case transitionMethod::image:
            presentation.frame =
                playlistTransitionFrame.load(
                    std::memory_order_acquire
                );
            break;

        case transitionMethod::vanillaDDS:
        case transitionMethod::blackFrame:
            break;
    }

    return presentation;
}

transitionMethod
manager::resolveTransitionMethod() const
{
    const auto fallback =
        configuredFallbackTransition();

    // First gate: playlist transition metadata has no authority at all when the
    // global permission switch is disabled.
    if (!config::usePlaylistTransitionMethod.GetValue())
    {
        return fallback;
    }

    // Second: a playlist must explicitly enumerate a transition method.
    if (!playbackSettings.transition)
    {
        return fallback;
    }

    // Image is playlist-only and requires a successfully pre-ingested still.
    if (*playbackSettings.transition == transitionMethod::image)
    {
        if (playlistTransitionFrame.load(
                std::memory_order_acquire))
        {
            return transitionMethod::image;
        }

        return fallback;
    }

    return *playbackSettings.transition;
}

void manager::beginDecoderTransition()
{
    activeTransitionMethod.store(
        resolveTransitionMethod(),
        std::memory_order_release
    );

    transitionActive.store(
        true,
        std::memory_order_release
    );
}

void manager::updateDecoderTransition()
{
    if (!transitionActive.load(
            std::memory_order_acquire))
    {
        return;
    }

    const auto frame =
        producerWorker.getLatestFrame();

    if (
        frame &&
        frame->sourceGeneration == sourceGeneration)
    {
        transitionActive.store(
            false,
            std::memory_order_release
        );
    }
}

void manager::cancelDecoderTransition()
{
    transitionActive.store(
        false,
        std::memory_order_release
    );
}

std::shared_ptr<const producedFrame>
manager::loadPlaylistTransitionImage(
    const std::string& imagePath)
{
    if (imagePath.empty())
        return nullptr;

    std::error_code statusError;
    if (!std::filesystem::is_regular_file(
            imagePath,
            statusError))
    {
        if (statusError)
        {
            REX::WARN(
                "Could not inspect playlist transition image '{}': {}.",
                imagePath,
                statusError.message()
            );
        }

        return nullptr;
    }

    decoder imageDecoder;

    if (
        !imageDecoder.open(imagePath.c_str()) ||
        !imageDecoder.initializeVideoDecoder())
    {
        return nullptr;
    }

    AVFrame* decoded = av_frame_alloc();
    if (decoded == nullptr)
        return nullptr;

    const auto decodeResult =
        imageDecoder.decodeNextFrame(decoded);

    std::shared_ptr<producedFrame> produced;

    if (decodeResult.status == decodeStatus::frameReady)
    {
        produced =
            imageDecoder.frameProduce(decoded);
    }

    av_frame_free(&decoded);

    if (produced)
    {
        // Transition images are presentation assets rather than timeline media.
        produced->timestamp = -1.0;
        produced->sourceGeneration = 0;

        REX::INFO(
            "Preloaded playlist transition image '{}': {}x{}.",
            imagePath,
            produced->width,
            produced->height
        );
    }

    return produced;
}

bool manager::transitionToSource(
    const std::string& nextPath)
{
    if (nextPath.empty())
    {
        return false;
    }

    lastSubmittedFrame = nullptr;

    const auto previousGeneration =
        sourceGeneration;

    ++sourceGeneration;
    beginDecoderTransition();

    if (nextPath == inputPath)
    {
        if (!decoderWorker.rewind())
        {
            sourceGeneration = previousGeneration;
            cancelDecoderTransition();
            return false;
        }

        REX::TRACE(
            "Manager rewound current source in place (generation {}).",
            sourceGeneration
        );
        return true;
    }

    if (!decoderWorker.switchSource(
            nextPath.c_str()))
    {
        sourceGeneration = previousGeneration;
        cancelDecoderTransition();
        return false;
    }

    inputPath = nextPath;

    REX::TRACE(
        "Manager transitioned to '{}' without restarting the producer/presentation path (generation {}).",
        inputPath,
        sourceGeneration
    );

    return true;
}

void manager::refillShuffleBag(
    bool initial)
{
    shuffleBag.clear();

    for (
        std::size_t index = 0;
        index < playbackSources.size();
        ++index)
    {
        if (
            initial &&
            index == currentSourceIndex)
        {
            continue;
        }

        shuffleBag.emplace_back(index);
    }

    std::shuffle(
        shuffleBag.begin(),
        shuffleBag.end(),
        shuffleRng
    );

    if (
        !initial &&
        shuffleBag.size() > 1 &&
        shuffleBag.back() == currentSourceIndex)
    {
        const auto different =
            std::find_if(
                shuffleBag.begin(),
                shuffleBag.end() - 1,
                [this](std::size_t index)
                {
                    return index !=
                        currentSourceIndex;
                }
            );

        if (different != shuffleBag.end() - 1)
        {
            std::iter_swap(
                different,
                shuffleBag.end() - 1
            );
        }
    }
}

std::optional<std::size_t>
manager::selectNextSource()
{
    if (playbackSources.empty())
    {
        return std::nullopt;
    }

    if (playbackSources.size() == 1)
    {
        if (looping.load(
                std::memory_order_acquire))
        {
            return currentSourceIndex;
        }

        return std::nullopt;
    }

    if (shuffleEnabled)
    {
        if (shuffleBag.empty())
        {
            if (!looping.load(
                    std::memory_order_acquire))
            {
                return std::nullopt;
            }

            refillShuffleBag(false);
        }

        if (shuffleBag.empty())
        {
            return std::nullopt;
        }

        const auto next =
            shuffleBag.back();

        shuffleBag.pop_back();

        return next;
    }

    if (
        currentSourceIndex + 1 <
        playbackSources.size())
    {
        return currentSourceIndex + 1;
    }

    if (looping.load(
            std::memory_order_acquire))
    {
        return std::size_t{0};
    }

    return std::nullopt;
}

void manager::run()
{
    while (!stopRequested)
    {
        auto frame =
            decoderWorker.getLatestFrame();

        if (frame && frame != lastSubmittedFrame)
        {
            producerWorker.submitFrame(
                frame,
                sourceGeneration
            );

            lastSubmittedFrame = frame;
        }

        // Do not clear a decoder-gap fallback merely because the decoder opened.
        // It remains active until the producer publishes a frame tagged with the
        // new source generation.
        updateDecoderTransition();

        if (!decoderWorker.isRunning())
        {
            const auto result =
                decoderWorker.getLastResult();

            switch (result.status)
            {
                case decodeStatus::endOfFile:
                {
                    const auto nextSource =
                        selectNextSource();

                    if (!nextSource)
                    {
                        stopRequested = true;
                        break;
                    }

                    const std::string nextPath =
                        playbackSources[*nextSource];

                    if (!transitionToSource(
                            nextPath))
                    {
                        REX::ERROR(
                            "Manager failed playlist transition from '{}' to '{}'.",
                            inputPath,
                            nextPath
                        );

                        stopRequested = true;
                        break;
                    }

                    currentSourceIndex =
                        *nextSource;

                    REX::INFO(
                        "Manager playlist transition: {}/{} '{}' using {} decoder-gap fallback.",
                        currentSourceIndex + 1,
                        playbackSources.size(),
                        inputPath,
                        transitionMethodName(
                            activeTransitionMethod.load(
                                std::memory_order_acquire
                            )
                        )
                    );

                    break;
                }

                case decodeStatus::ffmpegError:
                    stopRequested = true;
                    break;

                case decodeStatus::stopped:
                    stopRequested = true;
                    break;

                case decodeStatus::frameReady:
                    break;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)
        );
    }

    running = false;
}

void manager::stop()
{
    stopRequested = true;

    if (managerThread.joinable())
    {
        managerThread.join();
    }

    decoderWorker.stop();
    producerWorker.stop();

    cancelDecoderTransition();
    running = false;
}

manager::~manager()
{
    stop();
}

std::shared_ptr<manager> createManager(
    const char* inputPath,
    videoPlaybackSettings settings)
{
    auto newManager =
        std::make_shared<manager>();

    if (!newManager->start(
            inputPath,
            std::move(settings)))
    {
        return nullptr;
    }

    return newManager;
}

}
