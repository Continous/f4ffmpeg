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

        // Rewind the already-open decoder in place. Unlike start(), this does
        // not reopen the input, recreate the codec context, or rebuild the
        // hardware decode device. Intended for seamless looping.
        bool rewind();

        // Switch to a different input without touching manager's producer.
        // decoder::open() preserves the AVHWDeviceContext when possible.
        bool switchSource(const char* path);

        void stop();

        std::shared_ptr<const decodedFrame>
        getLatestFrame() const;

        bool isRunning() const;
        decodeResult getLastResult() const;

        private:
            void run();

            decoder workerDecoder;
            std::thread workerThread;

            std::atomic<bool> stopRequested = false;
            std::atomic<bool> running = false;

            std::atomic<std::shared_ptr<const decodedFrame>>
                latestFrame;

            std::atomic<decodeStatus> lastStatus =
                decodeStatus::stopped;

            std::atomic<int> lastFfmpegError = 0;
        };
}
