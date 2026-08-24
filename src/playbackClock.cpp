#include "pch.h"

#include "playbackClock.h"

#include <algorithm>
#include <cmath>

namespace f4ffmpeg
{
    namespace
    {
        constexpr double secondsPerDay =
            86400.0;

        // Calendar is float-backed and therefore not
        // precise enough to use tiny discontinuity
        // thresholds safely.
        //
        // A discontinuity of more than one world-minute
        // beyond expected timescale progression is large
        // enough to strongly imply wait/sleep/etc.
        constexpr double worldJumpThreshold =
            60.0;

        constexpr double timeScaleEpsilon =
            0.001;

        double sanitizeDelta(
            float value)
        {
            const double delta =
                static_cast<double>(value);

            if (
                !std::isfinite(delta) ||
                delta <= 0.0)
            {
                return 0.0;
            }

            return delta;
        }
    }


    playbackClock&
    playbackClock::get()
    {
        static playbackClock instance;
        return instance;
    }

    std::uint64_t
    playbackClock::getDiscontinuityCount() const
    {
        std::scoped_lock lock(mutex);

        return discontinuityCount;
    }

    void playbackClock::setMode(
        playbackClockMode newMode)
    {
        {
            std::scoped_lock lock(mutex);

            mode = newMode;

            // If we enter hybrid mode later, establish
            // a fresh Calendar baseline rather than
            // interpreting the mode change as a jump.
            calendarBaselineValid = false;
        }

        condition.notify_all();
    }


    bool playbackClock::setMode(
        std::string_view newMode)
    {
        if (
            newMode == "realtime" ||
            newMode == "real")
        {
            setMode(
                playbackClockMode::realtime
            );

            return true;
        }

        if (
            newMode == "game" ||
            newMode == "gametime")
        {
            setMode(
                playbackClockMode::game
            );

            return true;
        }

        if (newMode == "hybrid")
        {
            setMode(
                playbackClockMode::hybrid
            );

            return true;
        }

        return false;
    }


    playbackClockMode
    playbackClock::getMode() const
    {
        std::scoped_lock lock(mutex);

        return mode;
    }


    std::uint64_t
    playbackClock::getUpdateCount() const
    {
        std::scoped_lock lock(mutex);

        return updateCount;
    }


    bool playbackClock::waitForUpdate(
        std::uint64_t& lastUpdate,
        const std::atomic<bool>& stopRequested)
    {
        std::unique_lock lock(mutex);

        condition.wait(
            lock,
            [&]()
            {
                return
                    stopRequested.load(
                        std::memory_order_acquire
                    ) ||
                    updateCount != lastUpdate;
            }
        );

        if (stopRequested.load(
                std::memory_order_acquire))
        {
            return false;
        }

        lastUpdate = updateCount;

        return true;
    }

    void playbackClock::update(
        std::uint64_t expectedTimerTime)
    {
        auto* timer =
            RE::BSTimer::GetSingleton();

        if (timer == nullptr)
        {
            return;
        }

        const double gameDelta =
            sanitizeDelta(
                timer->delta
            );

        const double realDelta =
            sanitizeDelta(
                timer->realTimeDelta
            );

        auto* calendar =
            RE::Calendar::GetSingleton();

        bool calendarValid = false;

        double daysPassed = 0.0;
        double timeScale = 1.0;

        if (
            calendar != nullptr &&
            calendar->gameDaysPassed != nullptr &&
            calendar->timeScale != nullptr)
        {
            daysPassed =
                static_cast<double>(
                    calendar
                        ->gameDaysPassed
                        ->GetValue()
                );

            timeScale =
                static_cast<double>(
                    calendar
                        ->timeScale
                        ->GetValue()
                );

            calendarValid =
                std::isfinite(daysPassed) &&
                std::isfinite(timeScale) &&
                std::abs(timeScale) >
                    timeScaleEpsilon;
        }

        bool worldJumpDetected = false;
        double detectedWorldJump = 0.0;
        double expectedWorldAdvance = 0.0;

        {
            std::scoped_lock lock(mutex);

            ++updateCount;

            double advance = 0.0;

            switch (mode)
            {
                case playbackClockMode::realtime:
                {
                    advance =
                        realDelta;

                    break;
                }

                case playbackClockMode::game:
                {
                    advance =
                        gameDelta;

                    break;
                }

                case playbackClockMode::hybrid:
                {
                    advance =
                        gameDelta;

                    if (!calendarValid)
                    {
                        calendarBaselineValid =
                            false;

                        break;
                    }

                    if (!calendarBaselineValid)
                    {
                        lastDaysPassed =
                            daysPassed;

                        lastTimeScale =
                            timeScale;

                        calendarBaselineValid =
                            true;

                        break;
                    }

                    const bool timeScaleChanged =
                        std::abs(
                            timeScale -
                            lastTimeScale
                        ) >
                        timeScaleEpsilon;

                    const double worldDelta =
                        (
                            daysPassed -
                            lastDaysPassed
                        ) *
                        secondsPerDay;

                    if (
                        !timeScaleChanged &&
                        worldDelta > 0.0)
                    {
                        // Determine how much world time
                        // should reasonably have elapsed
                        // during this Fallout update at
                        // the current timescale.
                        //
                        // Using the larger delta prevents
                        // things like VATS/global timer
                        // manipulation from looking like a
                        // Calendar discontinuity.
                        const double expectedDelta =
                            std::max(
                                gameDelta,
                                realDelta
                            );

                        expectedWorldAdvance =
                            expectedDelta *
                            std::abs(timeScale);

                        const double unexpectedAdvance =
                            worldDelta -
                            expectedWorldAdvance;

                        if (
                            unexpectedAdvance >
                            worldJumpThreshold)
                        {
                            // Wait/sleep/etc. advanced
                            // Fallout's world clock
                            // discontinuously.
                            //
                            // In hybrid mode we consider
                            // that world time to have
                            // elapsed for the video too.
                            advance =
                                worldDelta;

                            worldJumpDetected =
                                true;

                            detectedWorldJump =
                                worldDelta;

                            ++discontinuityCount;
                        }
                    }

                    lastDaysPassed =
                        daysPassed;

                    lastTimeScale =
                        timeScale;

                    break;
                }
            }
            if (
                std::isfinite(advance) &&
                advance > 0.0)
            {
                clockTime +=
                    advance;
            }
        }

        if (worldJumpDetected)
        {
            REX::DEBUG(
                "Playback clock detected a world-time "
                "discontinuity of {:.3f}s "
                "(expected {:.3f}s at timescale {:.3f}).",
                detectedWorldJump,
                expectedWorldAdvance,
                timeScale
            );
        }
        condition.notify_all();
    }


    double playbackClock::now() const
    {
        std::scoped_lock lock(mutex);

        return clockTime;
    }


    bool playbackClock::waitUntil(
        double targetTime,
        const std::atomic<bool>& stopRequested)
    {
        if (!std::isfinite(targetTime))
        {
            return false;
        }

        std::unique_lock lock(mutex);

        condition.wait(
            lock,
            [&]()
            {
                return
                    stopRequested.load(
                        std::memory_order_acquire
                    ) ||
                    clockTime >= targetTime;
            }
        );

        return
            !stopRequested.load(
                std::memory_order_acquire
            );
    }


    void playbackClock::notifyWaiters()
    {
        condition.notify_all();
    }
}
