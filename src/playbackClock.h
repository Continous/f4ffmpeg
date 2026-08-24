#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <cstdint>

namespace f4ffmpeg
{
    enum class playbackClockMode
    {
        realtime,
        game,
        hybrid
    };

    class playbackClock
    {
    public:

        [[nodiscard]]
        std::uint64_t getDiscontinuityCount() const;

        [[nodiscard]]
        std::uint64_t getUpdateCount() const;

        bool waitForUpdate(
            std::uint64_t& lastUpdate,
            const std::atomic<bool>& stopRequested
        );

        static playbackClock& get();

        void setMode(
            playbackClockMode mode
        );

        bool setMode(
            std::string_view mode
        );

        [[nodiscard]]
        playbackClockMode getMode() const;

        // Fallout gameplay update point. Handled by playbackClockSource
        void update(
            double realSecondsElapsed,
            double gameHoursElapsed,
            double timeScale,
            bool discontinuity
        );

        // Absolute f4ffmpeg clock time, in seconds.
        [[nodiscard]]
        double now() const;

        // Wait until the Fallout-driven clock reaches
        // the requested absolute time.
        //
        // Returns false if the requesting worker was
        // asked to stop.
        bool waitUntil(
            double targetTime,
            const std::atomic<bool>& stopRequested
        );

        // Used when a worker changes its stop state so
        // it can wake immediately even if Fallout is
        // not currently updating.
        void notifyWaiters();

    private:

        std::uint64_t discontinuityCount = 0;
        std::uint64_t updateCount = 0;
        playbackClock() = default;

        playbackClock(
            const playbackClock&) = delete;

        playbackClock& operator=(
            const playbackClock&) = delete;

        mutable std::mutex mutex;
        std::condition_variable condition;

        playbackClockMode mode =
            playbackClockMode::hybrid;

        double clockTime = 0.0;
    };
}
