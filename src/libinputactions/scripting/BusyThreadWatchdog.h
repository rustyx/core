/*
    Input Actions - Input handler that executes user-defined actions
    Copyright (C) 2026 Marcin Woźniak

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <QObject>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>

class QThread;

namespace InputActions
{

/**
 * Detects the thread the watchdog was created on being stuck in a busy loop, e.g. a script that has entered an infinite loop.
 *
 * The thread is considered stuck when it has not returned to its event loop for longer than the timeout and has spent at least half of the timeout on a CPU
 * since then. A thread that is blocked waiting (e.g. for a D-Bus reply or an unresponsive compositor) is not considered stuck, as it will recover on its own.
 */
class BusyThreadWatchdog : public QObject
{
    Q_OBJECT

public:
    /**
     * @param onStuck Called on the watchdog thread, at most once each time the monitored thread gets stuck.
     */
    BusyThreadWatchdog(std::chrono::milliseconds timeout, std::function<void()> onStuck);
    ~BusyThreadWatchdog() override;

private:
    /**
     * Runs on the watchdog thread.
     */
    void check();
    /**
     * @return CPU time consumed by the monitored thread, or std::nullopt if it can't be determined.
     */
    std::optional<std::chrono::nanoseconds> monitoredThreadCpuTime() const;
    void resetBaseline(std::chrono::steady_clock::time_point time, std::optional<std::chrono::nanoseconds> cpuTime);

    std::chrono::milliseconds m_timeout;
    std::function<void()> m_onStuck;
    std::optional<clockid_t> m_monitoredThreadCpuClock;

    /**
     * Lives on the monitored thread, increments m_heartbeats whenever the thread processes events.
     */
    QTimer m_heartbeatTimer;
    std::atomic<uint64_t> m_heartbeats{};

    std::unique_ptr<QThread> m_thread;
    /**
     * Lives on the watchdog thread.
     */
    std::unique_ptr<QTimer> m_checkTimer;

    // Only accessed on the watchdog thread after it has started
    uint64_t m_lastHeartbeats{};
    std::chrono::steady_clock::time_point m_baselineTime;
    std::optional<std::chrono::nanoseconds> m_baselineCpuTime;
    bool m_stuckReported{};
};

}
