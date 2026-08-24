#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace f4ffmpeg
{
    class playbackClockSource
    {
    public:
        static playbackClockSource& get();

        bool start();
        void stop();

        [[nodiscard]]
        bool isRunning() const;

        ~playbackClockSource();

    private:
        playbackClockSource() = default;

        playbackClockSource(
            const playbackClockSource&
        ) = delete;

        playbackClockSource& operator=(
            const playbackClockSource&
        ) = delete;

        void run();

        std::thread workerThread;

        std::atomic<bool> stopRequested =
            false;

        std::atomic<bool> running =
            false;
    };
}
