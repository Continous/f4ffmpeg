#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "decoderWorker.h"
#include "playbackPolicy.h"
#include "producerWorker.h"

namespace f4ffmpeg
{
    class manager
    {
    public:
        ~manager();

        bool start(
            const char* inputPath,
            videoPlaybackSettings settings = {}
        );

        void stop();

        void setLooping(bool enabled)
        {
            looping = enabled;
        }

        std::shared_ptr<const producedFrame>
        getLatestFrame() const;

        transitionPresentation
        getTransitionPresentation() const;

    private:
        void run();

        // Unified playback transition. Rewinds same-source loops and switches
        // different playlist/shuffle sources without restarting the producer.
        bool transitionToSource(
            const std::string& nextPath
        );

        std::optional<std::size_t>
        selectNextSource();

        void refillShuffleBag(
            bool initial
        );

        transitionMethod
        resolveTransitionMethod() const;

        void beginDecoderTransition();
        void updateDecoderTransition();
        void cancelDecoderTransition();

        std::shared_ptr<const producedFrame>
        loadPlaylistTransitionImage(
            const std::string& imagePath
        );

        decodeWorker decoderWorker;
        producerWorker producerWorker;

        std::string inputPath;
        videoPlaybackSettings playbackSettings;

        std::vector<std::string> playbackSources;
        std::size_t currentSourceIndex = 0;

        bool shuffleEnabled = false;
        std::vector<std::size_t> shuffleBag;
        std::mt19937 shuffleRng{
            std::random_device{}()
        };

        std::atomic<bool> looping = false;

        // Generation 1 is the initially opened source. Every decoder handoff
        // increments this before the new source is opened/rewound.
        std::uint64_t sourceGeneration = 1;

        std::atomic<bool> transitionActive = false;
        std::atomic<transitionMethod> activeTransitionMethod =
            transitionMethod::vanillaDDS;
        std::atomic<
            std::shared_ptr<const producedFrame>
        > playlistTransitionFrame;

        std::thread managerThread;

        std::atomic<bool> stopRequested = false;
        std::atomic<bool> running = false;

        std::shared_ptr<const decodeWorker::decodedFrame>
            lastSubmittedFrame;
    };

    std::shared_ptr<manager> createManager(
        const char* inputPath,
        videoPlaybackSettings settings = {}
    );

    // Source-compatibility shim for pre-playlist call sites. The bool retains
    // its historical meaning (single-source looping) and is immediately
    // translated into the canonical settings object.
    std::shared_ptr<manager> createManager(
        const char* inputPath,
        bool looping
    );
}
