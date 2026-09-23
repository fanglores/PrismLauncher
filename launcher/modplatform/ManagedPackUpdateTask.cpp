// SPDX-License-Identifier: GPL-3.0-only

#include "ManagedPackUpdateTask.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QTimer>

#include "Application.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"

namespace {
constexpr qint64 kCacheLifetimeSecs = 12 * 60 * 60;
constexpr int kRemoteRequestIntervalMs = 2000;

QString cacheKey(const QString& type, const QString& packId)
{
    return type + '/' + packId;
}
}

bool ManagedPackUpdateTask::isSupportedRemote(const QString& type, const QString& packId, bool flameSupported)
{
    return !packId.isEmpty() && (type == "modrinth" || (type == "flame" && flameSupported));
}

bool ManagedPackUpdateTask::hasUpdate(const Instance& instance, const ModPlatform::IndexedVersion& latest)
{
    return instance.type == "modrinth" ? latest.version != instance.versionName
                                       : latest.fileId.toString() != instance.versionId;
}

ManagedPackUpdateTask::ManagedPackUpdateTask(QList<Instance> instances, bool forceRefresh) : m_forceRefresh(forceRefresh)
{
    QHash<QString, int> projects;
    for (auto& instance : instances) {
        const auto key = cacheKey(instance.type, instance.packId);
        if (!projects.contains(key)) {
            projects.insert(key, m_queue.size());
            m_queue.enqueue({ instance.type, instance.packId, {} });
        }
        m_queue[projects.value(key)].instances.append(std::move(instance));
    }
}

bool ManagedPackUpdateTask::abort()
{
    m_queue.clear();
    if (m_currentTask) {
        return m_currentTask->abort();
    }
    emitAborted();
    return true;
}

void ManagedPackUpdateTask::executeTask()
{
    checkNext();
}

void ManagedPackUpdateTask::checkNext()
{
    if (!isRunning()) {
        return;
    }
    if (m_queue.isEmpty()) {
        emitSucceeded();
        return;
    }

    const auto project = m_queue.dequeue();
    m_currentFinished = false;
    setStatus(tr("Checking modpack updates..."));

    const auto entry = APPLICATION->metacache()->resolveEntry("ManagedPackUpdates", cacheKey(project.type, project.packId) + ".json");
    if (m_forceRefresh) {
        entry->setStale(true);
    }
    m_currentRequestIsRemote = entry->isStale();

    ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks{};
    callbacks.onSucceed = [this, project, entry](auto& versions) {
        entry->setMaximumAge(kCacheLifetimeSecs);
        APPLICATION->metacache()->SaveEventually();
        const auto cachedAt = QFileInfo(entry->getFullPath()).lastModified().toSecsSinceEpoch();
        const auto checkedAt = m_currentRequestIsRemote || cachedAt <= 0 ? QDateTime::currentSecsSinceEpoch() : cachedAt;
        const auto expiresAt = checkedAt + kCacheLifetimeSecs;
        emit cacheUpdated(cacheKey(project.type, project.packId), expiresAt);

        for (const auto& instance : project.instances) {
            if (versions.isEmpty()) {
                emit updateChecked(instance.id, false, {}, {}, {}, checkedAt);
                continue;
            }

            const auto& latest = versions.constFirst();
            emit updateChecked(instance.id, hasUpdate(instance, latest), latest.version, latest.fileId.toString(),
                               QUrl(latest.downloadUrl), checkedAt);
        }
        finishCurrent();
    };
    callbacks.onFail = [this, project](const QString&, int) {
        for (const auto& instance : project.instances) {
            emit checkFailed(instance.id);
        }
        finishCurrent();
    };
    callbacks.onAbort = [this, project] {
        for (const auto& instance : project.instances) {
            emit checkFailed(instance.id);
        }
        finishCurrent();
    };

    const ResourceAPI::VersionSearchArgs args{
        .pack = std::make_shared<ModPlatform::IndexedPack>(ModPlatform::IndexedPack{ .addonId = project.packId }),
        .mcVersions = {},
        .loaders = {},
        .resourceType = ModPlatform::ResourceType::Modpack,
        .includeChangelog = false,
    };
    m_currentTask = project.type == "modrinth" ? ModrinthAPI::get().getProjectVersions(args, callbacks, entry)
                                                 : FlameAPI::get().getProjectVersions(args, callbacks, entry);
    if (!m_currentTask) {
        callbacks.onFail({}, -1);
        return;
    }
    connect(m_currentTask.get(), &Task::succeeded, this, [this, callbacks] {
        if (!m_currentFinished) {
            callbacks.onFail({}, -1);
        }
    });
    m_currentTask->start();
}

void ManagedPackUpdateTask::finishCurrent()
{
    if (m_currentFinished) {
        return;
    }
    m_currentFinished = true;
    const auto delay = m_currentRequestIsRemote ? kRemoteRequestIntervalMs : 0;
    QTimer::singleShot(delay, this, [this] {
        m_currentTask.reset();
        checkNext();
    });
}
