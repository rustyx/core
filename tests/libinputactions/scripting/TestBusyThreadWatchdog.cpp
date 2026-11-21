#include "Test.h"
#include <libinputactions/scripting/BusyThreadWatchdog.h>

namespace InputActions
{

static const std::chrono::milliseconds TIMEOUT{50};

/**
 * Keeps the current thread on the CPU without processing events.
 */
static void busyWait(std::chrono::milliseconds duration)
{
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
    }
}

class TestBusyThreadWatchdog : public Test
{
    Q_OBJECT

private slots:
    void eventLoopRunning_notReported()
    {
        std::atomic<int> reports{};
        BusyThreadWatchdog watchdog(TIMEOUT, [&reports]() {
            reports++;
        });

        QTest::qWait(TIMEOUT * 4);
        QCOMPARE(reports.load(), 0);
    }

    void blocked_notReported()
    {
        std::atomic<int> reports{};
        BusyThreadWatchdog watchdog(TIMEOUT, [&reports]() {
            reports++;
        });

        QThread::sleep(TIMEOUT * 4);
        QTest::qWait(TIMEOUT);
        QCOMPARE(reports.load(), 0);
    }

    void busyLoop_reportedOncePerStall()
    {
        std::atomic<int> reports{};
        BusyThreadWatchdog watchdog(TIMEOUT, [&reports]() {
            reports++;
        });

        busyWait(TIMEOUT * 6);
        QCOMPARE(reports.load(), 1);

        QTest::qWait(TIMEOUT * 2);
        QCOMPARE(reports.load(), 1);

        busyWait(TIMEOUT * 6);
        QCOMPARE(reports.load(), 2);
    }

    void busyLoopAfterBlocking_reported()
    {
        std::atomic<int> reports{};
        BusyThreadWatchdog watchdog(TIMEOUT, [&reports]() {
            reports++;
        });

        QThread::sleep(TIMEOUT * 3);
        busyWait(TIMEOUT * 6);
        QCOMPARE(reports.load(), 1);
    }

    /**
     * Simulates a busy loop that only gets a fraction of a CPU, e.g. due to contention or a CPU quota.
     */
    void busyLoopWithLowCpuShare_reported()
    {
        std::atomic<int> reports{};
        BusyThreadWatchdog watchdog(TIMEOUT, [&reports]() {
            reports++;
        });

        const auto end = std::chrono::steady_clock::now() + TIMEOUT * 12;
        while (std::chrono::steady_clock::now() < end) {
            busyWait(std::chrono::milliseconds(5));
            QThread::sleep(std::chrono::milliseconds(15));
        }
        QCOMPARE(reports.load(), 1);
    }
};

}

QTEST_MAIN(InputActions::TestBusyThreadWatchdog)
#include "TestBusyThreadWatchdog.moc"
