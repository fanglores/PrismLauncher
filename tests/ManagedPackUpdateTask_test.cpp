#include "modplatform/ManagedPackUpdateTask.h"

#include <QtTest>

class ManagedPackUpdateTaskTest : public QObject {
    Q_OBJECT

   private slots:
    void remoteEligibility()
    {
        QVERIFY(ManagedPackUpdateTask::isSupportedRemote("modrinth", "project", false));
        QVERIFY(ManagedPackUpdateTask::isSupportedRemote("flame", "123", true));
        QVERIFY(!ManagedPackUpdateTask::isSupportedRemote("flame", "123", false));
        QVERIFY(!ManagedPackUpdateTask::isSupportedRemote("modrinth", {}, true));
        QVERIFY(!ManagedPackUpdateTask::isSupportedRemote("flame", {}, true));
        QVERIFY(!ManagedPackUpdateTask::isSupportedRemote("unknown", "project", true));
    }

    void groupsCopiesByProviderAndProject()
    {
        QList<ManagedPackUpdateTask::Instance> instances{
            { "first", "modrinth", "same", "1", "1.0" },
            { "second", "modrinth", "same", "2", "2.0" },
            { "third", "flame", "same", "3", "3.0" },
            { "fourth", "modrinth", "different", "4", "4.0" },
        };
        ManagedPackUpdateTask task(instances);
        QCOMPARE(task.m_queue.size(), 3);
        QCOMPARE(task.m_queue.at(0).instances.size(), 2);
        QCOMPARE(task.m_queue.at(0).instances.at(0).id, QString("first"));
        QCOMPARE(task.m_queue.at(0).instances.at(1).id, QString("second"));
        QCOMPARE(task.m_queue.at(1).type, QString("flame"));
        QCOMPARE(task.m_queue.at(2).packId, QString("different"));
    }

    void emptyQueueFinishesWithoutNetwork()
    {
        ManagedPackUpdateTask task({});
        QSignalSpy finished(&task, &Task::finished);
        task.start();
        QCOMPARE(finished.size(), 1);
        QVERIFY(task.wasSuccessful());
    }

    void comparesProviderVersions()
    {
        ModPlatform::IndexedVersion latest;
        latest.fileId = QString("new-id");
        latest.version = "2.0";

        const ManagedPackUpdateTask::Instance modrinth{ "one", "modrinth", "project", "old-id", "1.0" };
        QVERIFY(ManagedPackUpdateTask::hasUpdate(modrinth, latest));
        latest.version = "1.0";
        QVERIFY(!ManagedPackUpdateTask::hasUpdate(modrinth, latest));

        const ManagedPackUpdateTask::Instance flame{ "two", "flame", "123", "old-id", "1.0" };
        QVERIFY(ManagedPackUpdateTask::hasUpdate(flame, latest));
        latest.fileId = QString("old-id");
        QVERIFY(!ManagedPackUpdateTask::hasUpdate(flame, latest));
    }
};

QTEST_GUILESS_MAIN(ManagedPackUpdateTaskTest)

#include "ManagedPackUpdateTask_test.moc"
