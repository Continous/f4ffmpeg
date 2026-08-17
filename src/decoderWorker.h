#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "decoder.h"

namespace f4ffmpeg
{
    class decodeWorker
    {
    public:
        struct decodedFrame
        {
            std::shared_ptr<AVFrame> frame;
            double timestamp = -1.0;
        };

        ~decodeWorker();

        bool start(const char* path);
        void stop();

        std::shared_ptr<const decodedFrame>
        getLatestFrame() const;

    private:
        void run();

        decoder workerDecoder;
        std::thread workerThread;

        std::atomic<bool> stopRequested = false;
        std::atomic<bool> running = false;

        std::atomic<std::shared_ptr<const decodedFrame>>
            latestFrame;

        decodeResult lastResult{
            decodeStatus::stopped,
            0
        };
    };
}
