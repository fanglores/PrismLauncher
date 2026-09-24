// SPDX-License-Identifier: GPL-3.0-only

#include "ManagedPackUpdateTask.h"

#include <algorithm>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTimer>

#include "Application.h"
#include "modplatform/ManagedPackUpdateScheduler.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"

namespace {
constexpr int kRemoteRequestIntervalMs = 2000;

QString cacheKey(const QString& type, const QString& packId)
{
    return type + '/' + packId;
}

ResourceAPI::VersionSearchArgs versionArgs(const QString& packId)
{
    return { .pack = std::make_shared<ModPlatform::IndexedPack>(ModPlatform::IndexedPack{ .addonId = packId }),
             .mcVersions = {},
             .loaders = {},
             .resourceType = ModPlatform::ResourceType::Modpack,
             .includeChangelog = false };
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

std::optional<QVector<ModPlatform::IndexedVersion>> ManagedPackUpdateTask::cachedVersions(const Instance& instance,
                                                                                          MetaEntryPtr entry)
{
    if (!entry || entry->isStale()) {
        return std::nullopt;
    }
    QFile file(entry->getFullPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const auto& api = instance.type == "modrinth" ? static_cast<const ResourceAPI&>(ModrinthAPI::get())
                                                   : static_cast<const ResourceAPI&>(FlameAPI::get());
    const auto versions = api.parseProjectVersions(file.readAll(), versionArgs(instance.packId));
    return versions ? std::optional(*versions) : std::nullopt;
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

void ManagedPackUpdateTask::enqueueManualCheck(Instance instance)
{
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue[i].type != instance.type || m_queue[i].packId != instance.packId) {
            continue;
        }
        auto project = m_queue.takeAt(i);
        if (std::ranges::none_of(project.instances, [&instance](const auto& queued) { return queued.id == instance.id; })) {
            project.instances.append(std::move(instance));
        }
        project.forceRefresh = true;
        m_queue.prepend(std::move(project));
        return;
    }
    m_queue.prepend({ instance.type, instance.packId, { std::move(instance) }, true });
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
    if (m_forceRefresh || project.forceRefresh) {
        entry->setStale(true);
    }
    m_currentRequestIsRemote = entry->isStale();

    ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks{};
    callbacks.onSucceed = [this, project, entry](auto& versions) {
        entry->setMaximumAge(ManagedPackUpdateScheduler::CacheLifetimeSecs);
        APPLICATION->metacache()->SaveEventually();
        const auto cachedAt = QFileInfo(entry->getFullPath()).lastModified().toSecsSinceEpoch();
        const auto checkedAt = m_currentRequestIsRemote || cachedAt <= 0 ? QDateTime::currentSecsSinceEpoch() : cachedAt;
        emit cacheUpdated(cacheKey(project.type, project.packId), checkedAt);

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
    const auto failProject = [this, project] {
        emit projectFailed(cacheKey(project.type, project.packId), QDateTime::currentSecsSinceEpoch());
        for (const auto& instance : project.instances) {
            emit checkFailed(instance.id);
        }
        finishCurrent();
    };
    callbacks.onFail = [failProject](const QString&, int) { failProject(); };
    callbacks.onAbort = failProject;

    const auto args = versionArgs(project.packId);
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
