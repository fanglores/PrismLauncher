// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QQueue>
#include <QString>
#include <QUrl>
#include <optional>

#include "modplatform/ModIndex.h"
#include "net/HttpMetaCache.h"
#include "tasks/Task.h"

class ManagedPackUpdateTask final : public Task {
    Q_OBJECT

   public:
    struct Instance {
        QString id;
        QString type;
        QString packId;
        QString versionId;
        QString versionName;
    };

    explicit ManagedPackUpdateTask(QList<Instance> instances, bool forceRefresh = false);
    ~ManagedPackUpdateTask() override = default;

    static bool isSupportedRemote(const QString& type, const QString& packId, bool flameSupported);
    static bool hasUpdate(const Instance& instance, const ModPlatform::IndexedVersion& latest);
    static std::optional<QVector<ModPlatform::IndexedVersion>> cachedVersions(const Instance& instance, MetaEntryPtr entry);
    void enqueueManualCheck(Instance instance);

   public slots:
    bool abort() override;

   protected slots:
    void executeTask() override;

   signals:
    void updateChecked(QString instanceId, bool available, QString version, QString versionId, QUrl downloadUrl, qint64 checkedAt);
    void checkFailed(QString instanceId);
    void cacheUpdated(QString cacheKey, qint64 checkedAt);
    void projectFailed(QString cacheKey, qint64 checkedAt);

   private:
    friend class ManagedPackUpdateTaskTest;

    struct Project {
        QString type;
        QString packId;
        QList<Instance> instances;
        bool forceRefresh = false;
    };

    void checkNext();
    void finishCurrent();

    QQueue<Project> m_queue;
    Task::Ptr m_currentTask;
    bool m_forceRefresh = false;
    bool m_currentFinished = false;
    bool m_currentRequestIsRemote = false;
};
