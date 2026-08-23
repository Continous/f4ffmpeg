#pragma once

#include <atomic>
#include <condition_variable>
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
            std::shared_ptr<const decodeWorker::decodedFrame> frame
        );

        std::shared_ptr<const producedFrame>
        getLatestFrame() const;

    private:
        void run();

        decoder producer;

        std::thread workerThread;

        std::atomic<bool> stopRequested = false;
        std::atomic<bool> running = false;

        std::atomic<
            std::shared_ptr<const decodeWorker::decodedFrame>
        > pendingFrame;

        std::atomic<
            std::shared_ptr<const producedFrame>
        > latestFrame;

        std::condition_variable wakeCondition;
        std::mutex wakeMutex;
    };
}
