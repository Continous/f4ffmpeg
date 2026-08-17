#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "decoderWorker.h"

namespace f4ffmpeg
{
    class producerWorker
    {
    public:
        ~producerWorker();

        bool start(const char* outputPath);
        void stop();

        void submitFrame(
            std::shared_ptr<const decodeWorker::decodedFrame> frame
        );

    private:
        void run();

        decoder producer;

        std::thread workerThread;

        std::atomic<bool> stopRequested = false;
        std::atomic<bool> running = false;

        std::atomic<
            std::shared_ptr<const decodeWorker::decodedFrame>
        > pendingFrame;

        std::condition_variable wakeCondition;
        std::mutex wakeMutex;

        std::string outputPath;
    };
}
