#include "modplatform/ManagedPackUpdateTask.h"

#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QTemporaryDir>

#include "net/MetaCacheSink.h"

class InspectableMetaCacheSink : public Net::MetaCacheSink {
   public:
    explicit InspectableMetaCacheSink(MetaEntryPtr entry)
        : Net::MetaCacheSink(std::move(entry), new Net::ChecksumValidator(QCryptographicHash::Md5))
    {}

    using Net::MetaCacheSink::initCache;
};

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

    void manualCheckPrioritizesAndRefreshesOnlySelectedProject()
    {
        ManagedPackUpdateTask task({ { "first", "modrinth", "first-project", "old", "1.0" },
                                     { "other-copy", "flame", "second-project", "41", "1.0" } });
        task.enqueueManualCheck({ "selected", "flame", "second-project", "42", "2.0" });

        QCOMPARE(task.m_queue.size(), 2);
        QCOMPARE(task.m_queue.at(0).type, QString("flame"));
        QCOMPARE(task.m_queue.at(0).instances.size(), 2);
        QCOMPARE(task.m_queue.at(0).instances.at(1).id, QString("selected"));
        QVERIFY(task.m_queue.at(0).forceRefresh);
        QVERIFY(!task.m_queue.at(1).forceRefresh);

        task.enqueueManualCheck({ "selected", "flame", "second-project", "42", "2.0" });
        QCOMPARE(task.m_queue.size(), 2);
        QCOMPARE(task.m_queue.at(0).instances.size(), 2);

        task.enqueueManualCheck({ "checked", "modrinth", "third-project", "old", "1.0" });
        QCOMPARE(task.m_queue.size(), 3);
        QCOMPARE(task.m_queue.at(0).instances.constFirst().id, QString("checked"));
        QVERIFY(task.m_queue.at(0).forceRefresh);
        QCOMPARE(task.m_queue.at(1).type, QString("flame"));
    }

    void forcedCheckDownloadsWithoutConditionalCacheHeaders()
    {
        QTemporaryDir directory(QDir::current().filePath("managed-pack-versions-XXXXXX"));
        QVERIFY(directory.isValid());
        HttpMetaCache cache;
        cache.addBase("ManagedPackUpdates", directory.path());
        auto entry = cache.resolveEntry("ManagedPackUpdates", "modrinth/project.json");
        QFile file(entry->getFullPath());
        QVERIFY(QDir().mkpath(QFileInfo(file).path()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("[]"), 2);
        file.close();
        entry->setStale(true);
        entry->setETag("\"old-etag\"");
        entry->setRemoteChangedTimestamp("Wed, 01 Jan 2025 00:00:00 GMT");

        ManagedPackUpdateTask::prepareCacheEntry(entry, false);
        InspectableMetaCacheSink sink(entry);
        QNetworkRequest conditional(QUrl("https://example.org/versions"));
        QCOMPARE(sink.initCache(conditional).value(), Net::Sink::InitType::Ok);
        QCOMPARE(conditional.rawHeader("If-None-Match"), QByteArray("\"old-etag\""));
        QVERIFY(conditional.hasRawHeader("If-Modified-Since"));

        ManagedPackUpdateTask::prepareCacheEntry(entry, true);
        QNetworkRequest full(QUrl("https://example.org/versions"));
        QCOMPARE(sink.initCache(full).value(), Net::Sink::InitType::Ok);
        QVERIFY(!full.hasRawHeader("If-None-Match"));
        QVERIFY(!full.hasRawHeader("If-Modified-Since"));
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

    void restoresAvailableUpdateFromCachedVersions()
    {
        QTemporaryDir directory(QDir::current().filePath("managed-pack-versions-XXXXXX"));
        QVERIFY(directory.isValid());
        HttpMetaCache cache(QDir(directory.path()).filePath("index.json"));
        cache.addBase("ManagedPackUpdates", directory.path());
        const ManagedPackUpdateTask::Instance instance{ "copy", "modrinth", "project", "old-id", "1.0" };
        auto entry = cache.resolveEntry("ManagedPackUpdates", "modrinth/project.json");
        QFile file(entry->getFullPath());
        QVERIFY(QDir().mkpath(QFileInfo(file).path()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray response = QByteArrayLiteral(
            "[{\"project_id\":\"project\",\"id\":\"older\",\"date_published\":\"2025-01-01T00:00:00Z\","
            "\"game_versions\":[\"1.20.1\"],\"loaders\":[],\"name\":\"1.0\",\"version_number\":\"1.0\","
            "\"version_type\":\"release\",\"files\":[{\"url\":\"https://example.org/old.mrpack\","
            "\"filename\":\"old.mrpack\",\"primary\":true,\"hashes\":{}}]},"
            "{\"project_id\":\"project\",\"id\":\"latest\",\"date_published\":\"2025-02-01T00:00:00Z\","
            "\"game_versions\":[\"1.20.1\"],\"loaders\":[],\"name\":\"2.0\",\"version_number\":\"2.0\","
            "\"version_type\":\"release\",\"files\":[{\"url\":\"https://example.org/new.mrpack\","
            "\"filename\":\"new.mrpack\",\"primary\":true,\"hashes\":{}}]}]");
        QCOMPARE(file.write(response), response.size());
        file.close();
        entry->setStale(false);
        entry->setLocalChangedTimestamp(QFileInfo(file).lastModified().toUTC().toMSecsSinceEpoch());
        entry->setMaximumAge(12 * 60 * 60);
        QVERIFY(cache.updateEntry(entry));
        cache.SaveNow();

        HttpMetaCache reopened(QDir(directory.path()).filePath("index.json"));
        reopened.addBase("ManagedPackUpdates", directory.path());
        reopened.Load();
        entry = reopened.resolveEntry("ManagedPackUpdates", "modrinth/project.json");
        QVERIFY(!entry->isStale());

        const auto versions = ManagedPackUpdateTask::cachedVersions(instance, entry);
        QVERIFY(versions.has_value());
        QCOMPARE(versions->size(), 2);
        const auto& latest = versions->constFirst();
        QCOMPARE(latest.version, QString("2.0"));
        QCOMPARE(latest.fileId.toString(), QString("latest"));
        QCOMPARE(latest.downloadUrl, QString("https://example.org/new.mrpack"));
        QVERIFY(ManagedPackUpdateTask::hasUpdate(instance, latest));

        entry->setStale(true);
        QVERIFY(!ManagedPackUpdateTask::cachedVersions(instance, entry));
        entry->setStale(false);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("not json"), 8);
        file.close();
        QVERIFY(!ManagedPackUpdateTask::cachedVersions(instance, entry));
    }

    void restoresCurseForgeUpdateFromCachedVersions()
    {
        QTemporaryDir directory(QDir::current().filePath("managed-pack-versions-XXXXXX"));
        QVERIFY(directory.isValid());
        HttpMetaCache cache;
        cache.addBase("ManagedPackUpdates", directory.path());
        const ManagedPackUpdateTask::Instance instance{ "copy", "flame", "123", "41", "1.0" };
        auto entry = cache.resolveEntry("ManagedPackUpdates", "flame/123.json");
        QFile file(entry->getFullPath());
        QVERIFY(QDir().mkpath(QFileInfo(file).path()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray response = QByteArrayLiteral(
            "{\"data\":[{\"modId\":123,\"id\":42,\"gameVersions\":[\"1.20.1\"],"
            "\"fileDate\":\"2025-02-01T00:00:00Z\",\"displayName\":\"2.0\","
            "\"fileName\":\"pack.zip\",\"releaseType\":1,"
            "\"downloadUrl\":\"https://example.org/pack.zip\"}]}");
        QCOMPARE(file.write(response), response.size());
        file.close();
        entry->setStale(false);

        const auto versions = ManagedPackUpdateTask::cachedVersions(instance, entry);
        QVERIFY(versions.has_value());
        QCOMPARE(versions->size(), 1);
        QCOMPARE(versions->constFirst().fileId.toString(), QString("42"));
        QVERIFY(ManagedPackUpdateTask::hasUpdate(instance, versions->constFirst()));
    }
};

QTEST_GUILESS_MAIN(ManagedPackUpdateTaskTest)

#include "ManagedPackUpdateTask_test.moc"
