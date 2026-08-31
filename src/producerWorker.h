#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "decoderWorker.h"

namespace f4ffmpeg
{
    class producerWorker
    {
    public:
        ~producerWorker();

        bool start();
        void stop();

        void submitFrame(
            std::shared_ptr<const decodeWorker::decodedFrame> frame,
            std::uint64_t sourceGeneration
        );

        std::shared_ptr<const producedFrame>
        getLatestFrame() const;

    private:
        struct frameSubmission
        {
            std::shared_ptr<const decodeWorker::decodedFrame> frame;
            std::uint64_t sourceGeneration = 0;
        };

        void run();

        decoder producer;

        std::thread workerThread;

        std::atomic<bool> stopRequested = false;
        std::atomic<bool> running = false;

        std::atomic<
            std::shared_ptr<const frameSubmission>
        > pendingFrame;

        std::atomic<
            std::shared_ptr<const producedFrame>
        > latestFrame;

        std::condition_variable wakeCondition;
        std::mutex wakeMutex;
    };
}
