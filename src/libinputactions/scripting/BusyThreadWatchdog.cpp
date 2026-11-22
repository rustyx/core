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

#include "BusyThreadWatchdog.h"
#include <QFile>
#include <QThread>
#include <cstring>
#include <libinputactions/globals.h>
#include <pthread.h>
#include <unistd.h>

namespace InputActions
{

/**
 * Fraction of the timeout the monitored thread must have spent on a CPU since it last processed events for it to be considered stuck. A thread blocked in a
 * system call uses close to none, regardless of how long it's blocked for.
 */
static constexpr double BUSY_CPU_TIME_FRACTION = 0.5;

static long long toMilliseconds(std::chrono::nanoseconds duration)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

BusyThreadWatchdog::BusyThreadWatchdog(std::chrono::milliseconds timeout, std::function<void()> onStuck)
    : m_timeout(timeout)
    , m_onStuck(std::move(onStuck))
    , m_monitoredThreadId(gettid())
    , m_thread(std::make_unique<QThread>())
    , m_checkTimer(std::make_unique<QTimer>())
{
    clockid_t clock{};
    if (const auto error = pthread_getcpuclockid(pthread_self(), &clock); error == 0) {
        m_monitoredThreadCpuClock = clock;
    } else {
        qWarning(INPUTACTIONS, "Failed to get the CPU clock of the monitored thread, every stall will be treated as a busy loop: %s", strerror(error));
    }
    const auto now = std::chrono::steady_clock::now();
    resetBaseline(now, monitoredThreadCpuTime());
    m_lastCheckTime = now;
    m_blockedSince = now;

    const auto interval = m_timeout / 4;
    connect(&m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        m_heartbeats.fetch_add(1, std::memory_order_relaxed);
    });
    m_heartbeatTimer.start(interval);

    m_checkTimer->setInterval(interval);
    m_checkTimer->moveToThread(m_thread.get());
    connect(m_checkTimer.get(), &QTimer::timeout, m_checkTimer.get(), [this]() {
        check();
    });
    connect(m_thread.get(), &QThread::started, m_checkTimer.get(), qOverload<>(&QTimer::start));
    m_thread->start();
}

BusyThreadWatchdog::~BusyThreadWatchdog()
{
    QMetaObject::invokeMethod(m_checkTimer.get(), &QTimer::stop, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

void BusyThreadWatchdog::check()
{
    const auto heartbeats = m_heartbeats.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const auto cpuTime = monitoredThreadCpuTime();

    // If the watchdog thread didn't run either, e.g. because all processes were frozen while the system was suspending or resuming, the monitored thread
    // couldn't have processed events regardless of what it was doing
    const auto watchdogDelayed = now - m_lastCheckTime > m_checkTimer->intervalAsDuration() * 2;
    m_lastCheckTime = now;

    if (heartbeats != m_lastHeartbeats) {
        if (m_blockedReported) {
            qWarning(INPUTACTIONS, "Thread %d is processing events again after roughly %lld ms", m_monitoredThreadId, toMilliseconds(now - m_blockedSince));
        }
        m_lastHeartbeats = heartbeats;
        m_stuckReported = false;
        m_blockedReported = false;
        m_blockedSince = now;
        resetBaseline(now, cpuTime);
        return;
    }
    if (watchdogDelayed) {
        m_blockedSince = now;
    }

    if (m_stuckReported || now - m_baselineTime < m_timeout) {
        return;
    }

    // CPU time is accumulated until the thread processes events, rather than compared to the elapsed time, so that a busy loop that only gets a fraction of a
    // CPU (e.g. due to contention or a CPU quota) is still detected, just later
    if (cpuTime && m_baselineCpuTime && *cpuTime - *m_baselineCpuTime < m_timeout * BUSY_CPU_TIME_FRACTION) {
        if (!m_blockedReported && now - m_blockedSince >= m_timeout) {
            m_blockedReported = true;
            qWarning(INPUTACTIONS,
                     "Thread %d has not processed events for %lld ms, using %lld ms of CPU time (%s)",
                     m_monitoredThreadId,
                     toMilliseconds(now - m_blockedSince),
                     toMilliseconds(*cpuTime - *m_baselineCpuTime),
                     qUtf8Printable(threadWaitState(m_monitoredThreadId)));
        }
        return;
    }

    m_stuckReported = true;
    m_onStuck();
}

QString BusyThreadWatchdog::threadWaitState(pid_t threadId)
{
    // /proc/self/task/<tid>/syscall would be more useful, but it's not readable by setgid processes
    const auto directory = QString("/proc/self/task/%1/").arg(threadId);

    QString state = "unknown";
    QFile statFile(directory + "stat");
    if (statFile.open(QIODevice::ReadOnly)) {
        // The state follows the thread name, which is enclosed in parentheses and may contain any character
        const auto stat = QString::fromUtf8(statFile.readAll());
        if (const auto nameEnd = stat.lastIndexOf(')'); nameEnd != -1 && nameEnd + 2 < stat.size()) {
            state = stat.at(nameEnd + 2);
        }
    }

    QString wchan = "unknown";
    QFile wchanFile(directory + "wchan");
    if (wchanFile.open(QIODevice::ReadOnly)) {
        wchan = QString::fromUtf8(wchanFile.readAll()).trimmed();
    }

    return QString("state %1, wchan %2").arg(state, wchan);
}

std::optional<std::chrono::nanoseconds> BusyThreadWatchdog::monitoredThreadCpuTime() const
{
    timespec time{};
    if (!m_monitoredThreadCpuClock || clock_gettime(*m_monitoredThreadCpuClock, &time) != 0) {
        return {};
    }
    return std::chrono::seconds(time.tv_sec) + std::chrono::nanoseconds(time.tv_nsec);
}

void BusyThreadWatchdog::resetBaseline(std::chrono::steady_clock::time_point time, std::optional<std::chrono::nanoseconds> cpuTime)
{
    m_baselineTime = time;
    m_baselineCpuTime = cpuTime;
}

}
