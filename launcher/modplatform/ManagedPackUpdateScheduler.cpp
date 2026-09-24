// SPDX-License-Identifier: GPL-3.0-only

#include "ManagedPackUpdateScheduler.h"

#include <algorithm>
#include <limits>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

ManagedPackUpdateScheduler::ManagedPackUpdateScheduler(QObject* parent) : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { onTimeout(QDateTime::currentSecsSinceEpoch()); });
}

void ManagedPackUpdateScheduler::onTimeout(qint64 now)
{
    m_timer.stop();
    if (m_busy || m_results.empty()) {
        return;
    }
    const auto nextExpiry = std::ranges::min_element(m_results, {}, [](const auto& entry) {
                                return entry.second.checkedAt;
                            })->second.checkedAt + CacheLifetimeSecs;
    if (nextExpiry > now) {
        schedule();
    } else {
        emit checkDue();
    }
}

void ManagedPackUpdateScheduler::setCacheDirectory(QString path)
{
    m_timer.stop();
    m_results.clear();
    m_loadedKeys.clear();
    m_cacheDirectory = path;
}

QString ManagedPackUpdateScheduler::resultPath(const QString& cacheKey) const
{
    const auto name = QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(m_cacheDirectory).filePath(QString::fromLatin1(name) + ".status.json");
}

std::optional<ManagedPackUpdateScheduler::Result> ManagedPackUpdateScheduler::result(const QString& cacheKey)
{
    if (const auto it = m_results.find(cacheKey); it != m_results.end()) {
        return it->second;
    }
    if (!m_loadedKeys.insert(cacheKey).second || m_cacheDirectory.isEmpty()) {
        return std::nullopt;
    }

    QFile file(resultPath(cacheKey));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    const auto object = document.object();
    const auto checkedAt = object.value("checkedAt").toInteger(-1);
    if (object.value("key").toString() != cacheKey || !object.value("success").isBool() || checkedAt <= 0 ||
        checkedAt > std::numeric_limits<qint64>::max() - CacheLifetimeSecs) {
        return std::nullopt;
    }
    const Result value{ object.value("success").toBool(), checkedAt };
    m_results[cacheKey] = value;
    schedule();
    return value;
}

bool ManagedPackUpdateScheduler::needsCheck(const QString& cacheKey, qint64 now, bool forceRefresh)
{
    const auto cached = result(cacheKey);
    return forceRefresh || !cached || cached->checkedAt + CacheLifetimeSecs <= now;
}

void ManagedPackUpdateScheduler::recordResult(const QString& cacheKey, bool success, qint64 checkedAt)
{
    m_loadedKeys.insert(cacheKey);
    m_results[cacheKey] = { success, checkedAt };
    if (!m_cacheDirectory.isEmpty() && QDir().mkpath(m_cacheDirectory)) {
        QSaveFile file(resultPath(cacheKey));
        if (file.open(QIODevice::WriteOnly)) {
            const QJsonObject object{ { "key", cacheKey }, { "success", success }, { "checkedAt", checkedAt } };
            const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
            if (file.write(data) == data.size()) {
                file.commit();
            }
        }
    }
    schedule();
}

void ManagedPackUpdateScheduler::retainProjects(const std::set<QString>& cacheKeys)
{
    const auto removedResults = std::erase_if(m_results, [&cacheKeys](const auto& entry) {
        return !cacheKeys.contains(entry.first);
    });
    std::erase_if(m_loadedKeys, [&cacheKeys](const auto& key) { return !cacheKeys.contains(key); });
    if (removedResults != 0) {
        schedule();
    }
}

void ManagedPackUpdateScheduler::setBusy(bool busy)
{
    m_busy = busy;
    schedule();
}

void ManagedPackUpdateScheduler::schedule()
{
    m_timer.stop();
    if (m_busy || m_results.empty()) {
        return;
    }

    const auto now = QDateTime::currentSecsSinceEpoch();
    const auto nextExpiry = std::ranges::min_element(m_results, {}, [](const auto& entry) {
                                return entry.second.checkedAt;
                            })->second.checkedAt + CacheLifetimeSecs;
    if (nextExpiry <= now) {
        m_timer.start(0);
        return;
    }

    const auto remainingSecs = nextExpiry - now;
    const auto maxIntervalMs = std::numeric_limits<int>::max();
    const auto intervalMs = remainingSecs >= maxIntervalMs / 1000 ? maxIntervalMs : static_cast<int>(remainingSecs * 1000);
    m_timer.start(intervalMs);
}
