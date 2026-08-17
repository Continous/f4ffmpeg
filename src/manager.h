#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "decoderWorker.h"
#include "producerWorker.h"

namespace f4ffmpeg
{
    class manager
    {
    public:
        ~manager();

        bool start(
            const char* inputPath,
            const char* outputPath
        );

        void stop();

        void manager::setLooping(bool enabled)
        {
            looping = enabled;
        }

    private:
        void run();

        decodeWorker decoderWorker;
        producerWorker producerWorker;

        std::string inputPath;
        bool looping = false;

        std::thread managerThread;

        std::atomic<bool> stopRequested = false;
        std::atomic<bool> running = false;

        std::shared_ptr<const decodeWorker::decodedFrame>
            lastSubmittedFrame;
    };

    std::shared_ptr<manager> createManager(
        const char* inputPath,
        const char* outputPath
    );
}
