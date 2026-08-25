#include "pch.h"

#include "playbackClockSource.h"
#include "playbackClock.h"

#include <chrono>
#include <cmath>

namespace f4ffmpeg
{
    playbackClockSource&
    playbackClockSource::get()
    {
        static playbackClockSource instance;
        return instance;
    }


    bool playbackClockSource::start()
    {
        if (running.load(
                std::memory_order_acquire))
        {
            return false;
        }

        if (workerThread.joinable())
        {
            workerThread.join();
        }

        stopRequested.store(
            false,
            std::memory_order_release
        );

        running.store(
            true,
            std::memory_order_release
        );

        workerThread =
            std::thread(
                &playbackClockSource::run,
                this
            );

        return true;
    }

void playbackClockSource::run()
{
    constexpr double discontinuityThresholdHours =
        0.25; //Wait time increments in bursts of .25 game hours.

    bool baselineValid = false;

    std::uint32_t previousMidnights = 0;
    double previousHour = 0.0;

    auto previousRealTime =
        std::chrono::steady_clock::now();

    while (!stopRequested.load(
        std::memory_order_acquire))
    {
        const auto currentRealTime =
            std::chrono::steady_clock::now();

        const double realSecondsElapsed =
            std::chrono::duration<double>(
                currentRealTime -
                previousRealTime
            ).count();

        previousRealTime =
            currentRealTime;

        auto* calendar =
            RE::Calendar::GetSingleton();

        if (
            calendar == nullptr ||
            calendar->gameHour == nullptr ||
            calendar->timeScale == nullptr)
        {
            baselineValid = false;

            playbackClock::get()
                .update(
                    realSecondsElapsed,
                    0.0,
                    0.0,
                    false
                );

            std::this_thread::sleep_for(
                std::chrono::milliseconds(2)
            );

            continue;
        }

        const std::uint32_t currentMidnights =
            calendar->midnightsPassed;

        const double currentHour =
            static_cast<double>(
                calendar->gameHour->GetValue()
            );

        const double timeScale =
            static_cast<double>(
                calendar->timeScale->GetValue()
            );

        if (
            !std::isfinite(currentHour) ||
            !std::isfinite(timeScale))
        {
            baselineValid = false;

            playbackClock::get()
                .update(
                    realSecondsElapsed,
                    0.0,
                    0.0,
                    false
                );

            std::this_thread::sleep_for(
                std::chrono::milliseconds(2)
            );

            continue;
        }

        double gameHoursElapsed = 0.0;
        bool discontinuity = false;

        if (!baselineValid)
        {
            previousMidnights =
                currentMidnights;

            previousHour =
                currentHour;

            baselineValid =
                true;

            REX::TRACE(
                "Playback clock source established "
                "Calendar baseline: midnight={}, "
                "hour={:.6f}, timescale={:.3f}.",
                currentMidnights,
                currentHour,
                timeScale
            );
        }
        else
        {
            const auto midnightDelta =
                static_cast<std::int64_t>(
                    currentMidnights
                ) -
                static_cast<std::int64_t>(
                    previousMidnights
                );

            const double calendarDelta =
                (
                    currentHour -
                    previousHour
                ) +
                (
                    static_cast<double>(
                        midnightDelta
                    ) *
                    24.0
                );

            /*
             * Always consume the current sample.
             * A backwards jump establishes a new baseline
             * without advancing playback.
             */
            previousMidnights =
                currentMidnights;

            previousHour =
                currentHour;

            if (calendarDelta > 0.0)
            {
                gameHoursElapsed =
                    calendarDelta;

                discontinuity =
                    gameHoursElapsed >
                        discontinuityThresholdHours;

                REX::TRACE(
                    "Calendar advanced {:.6f} game hours "
                    "(midnight={}, hour={:.6f}, "
                    "timescale={:.3f}, discontinuity={}).",
                    gameHoursElapsed,
                    currentMidnights,
                    currentHour,
                    timeScale,
                    discontinuity
                );
            }
        }

        playbackClock::get()
            .update(
                realSecondsElapsed,
                gameHoursElapsed,
                timeScale,
                discontinuity
            );

        std::this_thread::sleep_for(
            std::chrono::milliseconds(2)
        );
    }

    running.store(
        false,
        std::memory_order_release
    );
}


    void playbackClockSource::stop()
    {
        stopRequested.store(
            true,
            std::memory_order_release
        );

        if (workerThread.joinable())
        {
            workerThread.join();
        }

        running.store(
            false,
            std::memory_order_release
        );
    }


    bool playbackClockSource::isRunning() const
    {
        return running.load(
            std::memory_order_acquire
        );
    }


    playbackClockSource::~playbackClockSource()
    {
        stop();
    }
}
