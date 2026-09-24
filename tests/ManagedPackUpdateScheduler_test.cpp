#include <QtTest>

#include <limits>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "modplatform/ManagedPackUpdateScheduler.h"
#include "tasks/Task.h"

class PendingUpdateTask final : public Task {
   public:
    void complete() { emitSucceeded(); }

   protected:
    void executeTask() override {}
};

class ManagedPackUpdateSchedulerTest : public QObject {
    Q_OBJECT

   private slots:
    void successfulCacheExpiresAndRequestsCheck()
    {
        ManagedPackUpdateScheduler scheduler;
        QSignalSpy due(&scheduler, &ManagedPackUpdateScheduler::checkDue);
        PendingUpdateTask task;
        QSignalSpy started(&task, &Task::started);
        const auto key = QStringLiteral("modrinth/project");
        const auto expiry = QDateTime::currentSecsSinceEpoch() + 2;
        auto now = expiry - 1;

        connect(&scheduler, &ManagedPackUpdateScheduler::checkDue, &task, [&] {
            if (scheduler.needsCheck(key, now)) {
                scheduler.setBusy(true);
                task.start();
            }
        });

        scheduler.setBusy(true);
        scheduler.recordResult(key, true, expiry - ManagedPackUpdateScheduler::CacheLifetimeSecs);
        QVERIFY(!scheduler.needsCheck(key, expiry - 1));
        QVERIFY(scheduler.needsCheck(key, expiry));
        QCOMPARE(due.size(), 0);

        scheduler.setBusy(false);
        scheduler.onTimeout(now);
        QCOMPARE(due.size(), 0);
        now = expiry;
        scheduler.onTimeout(now);
        QCOMPARE(due.size(), 1);
        QCOMPARE(started.size(), 1);
        QVERIFY(task.isRunning());

        task.complete();
        scheduler.recordResult(key, true, QDateTime::currentSecsSinceEpoch());
        scheduler.setBusy(false);
        QVERIFY(!scheduler.needsCheck(key));
        scheduler.onTimeout(now);
        QCOMPARE(due.size(), 1);
    }

    void failedCacheExpiresAndRequestsCheck()
    {
        ManagedPackUpdateScheduler scheduler;
        QSignalSpy due(&scheduler, &ManagedPackUpdateScheduler::checkDue);
        const auto key = QStringLiteral("flame/123");
        const auto expiry = QDateTime::currentSecsSinceEpoch() + 2;

        scheduler.recordResult(key, false, expiry - ManagedPackUpdateScheduler::CacheLifetimeSecs);
        QVERIFY(!scheduler.result(key)->success);
        QVERIFY(!scheduler.needsCheck(key));
        scheduler.onTimeout(expiry - 1);
        QCOMPARE(due.size(), 0);
        scheduler.onTimeout(expiry);
        QCOMPARE(due.size(), 1);

        scheduler.setBusy(true);
        scheduler.recordResult(key, false, QDateTime::currentSecsSinceEpoch());
        scheduler.setBusy(false);
        scheduler.onTimeout(expiry);
        QCOMPARE(due.size(), 1);
        QVERIFY(!scheduler.needsCheck(key));
    }

    void startupRefreshBypassesFreshResults()
    {
        ManagedPackUpdateScheduler scheduler;
        QSignalSpy due(&scheduler, &ManagedPackUpdateScheduler::checkDue);
        const auto now = QDateTime::currentSecsSinceEpoch();

        scheduler.recordResult("modrinth/success", true, now);
        scheduler.recordResult("flame/failure", false, now);
        for (const auto& key : { "modrinth/success", "flame/failure" }) {
            QVERIFY(!scheduler.needsCheck(key, now));
            QVERIFY(scheduler.needsCheck(key, now, true));
        }
        QCOMPARE(due.size(), 0);
    }

    void resultsAreSharedAcrossRestarts()
    {
        QTemporaryDir directory(QDir::current().filePath("managed-pack-updates-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto key = QStringLiteral("modrinth/shared");
        const auto checkedAt = QDateTime::currentSecsSinceEpoch();

        {
            ManagedPackUpdateScheduler scheduler;
            scheduler.setCacheDirectory(directory.path());
            scheduler.recordResult(key, false, checkedAt);
        }
        {
            ManagedPackUpdateScheduler scheduler;
            scheduler.setCacheDirectory(directory.path());
            const auto cached = scheduler.result(key);
            QVERIFY(cached.has_value());
            QVERIFY(!cached->success);
            QCOMPARE(cached->checkedAt, checkedAt);
            QVERIFY(!scheduler.needsCheck(key));
            scheduler.recordResult(key, true, checkedAt);
        }
        {
            ManagedPackUpdateScheduler scheduler;
            scheduler.setCacheDirectory(directory.path());
            QVERIFY(scheduler.result(key)->success);
            QVERIFY(!scheduler.needsCheck(key));
            QVERIFY(scheduler.needsCheck("modrinth/other"));
        }

        const auto files = QDir(directory.path()).entryList({ "*.status.json" }, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file(QDir(directory.path()).filePath(files.first()));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("{}"), 2);
        file.close();

        ManagedPackUpdateScheduler scheduler;
        scheduler.setCacheDirectory(directory.path());
        QVERIFY(!scheduler.result(key).has_value());
        QVERIFY(scheduler.needsCheck(key));
    }

    void expiredFailureIsRecheckedAfterRestart()
    {
        QTemporaryDir directory(QDir::current().filePath("managed-pack-updates-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto key = QStringLiteral("flame/123");

        {
            ManagedPackUpdateScheduler scheduler;
            scheduler.setCacheDirectory(directory.path());
            scheduler.recordResult(key, false, QDateTime::currentSecsSinceEpoch() - ManagedPackUpdateScheduler::CacheLifetimeSecs - 1);
        }

        ManagedPackUpdateScheduler scheduler;
        scheduler.setCacheDirectory(directory.path());
        QSignalSpy due(&scheduler, &ManagedPackUpdateScheduler::checkDue);
        QVERIFY(!scheduler.result(key)->success);
        QVERIFY(scheduler.needsCheck(key));
        scheduler.onTimeout(QDateTime::currentSecsSinceEpoch());
        QCOMPARE(due.size(), 1);
    }

    void busyAndRemovedProjectsDoNotRequestChecks()
    {
        ManagedPackUpdateScheduler scheduler;
        QSignalSpy due(&scheduler, &ManagedPackUpdateScheduler::checkDue);
        const auto removed = QStringLiteral("modrinth/removed");
        const auto expired = QStringLiteral("modrinth/expired");
        const auto checkedAt = QDateTime::currentSecsSinceEpoch() - ManagedPackUpdateScheduler::CacheLifetimeSecs - 1;

        scheduler.setBusy(true);
        scheduler.recordResult(removed, true, checkedAt);
        scheduler.recordResult(expired, false, checkedAt);
        scheduler.retainProjects({ expired });
        scheduler.onTimeout(QDateTime::currentSecsSinceEpoch());
        QCOMPARE(due.size(), 0);

        scheduler.setBusy(false);
        scheduler.onTimeout(QDateTime::currentSecsSinceEpoch());
        QCOMPARE(due.size(), 1);
        QVERIFY(scheduler.needsCheck(expired));
        QVERIFY(scheduler.needsCheck(removed));
    }

    void distantExpiryDoesNotOverflowTimer()
    {
        ManagedPackUpdateScheduler scheduler;
        QSignalSpy due(&scheduler, &ManagedPackUpdateScheduler::checkDue);
        const auto key = QStringLiteral("modrinth/distant");

        scheduler.recordResult(key, true, std::numeric_limits<qint64>::max() - ManagedPackUpdateScheduler::CacheLifetimeSecs);
        QVERIFY(!scheduler.needsCheck(key));
        scheduler.onTimeout(QDateTime::currentSecsSinceEpoch());
        QCOMPARE(due.size(), 0);
    }
};

QTEST_GUILESS_MAIN(ManagedPackUpdateSchedulerTest)

#include "ManagedPackUpdateScheduler_test.moc"
