// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <map>
#include <optional>
#include <set>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

class ManagedPackUpdateScheduler : public QObject {
    Q_OBJECT

   public:
    explicit ManagedPackUpdateScheduler(QObject* parent = nullptr);

    static constexpr qint64 CacheLifetimeSecs = 12 * 60 * 60;

    struct Result {
        bool success;
        qint64 checkedAt;
    };

    void setCacheDirectory(QString path);
    std::optional<Result> result(const QString& cacheKey);
    bool needsCheck(const QString& cacheKey, qint64 now = QDateTime::currentSecsSinceEpoch());
    void recordResult(const QString& cacheKey, bool success, qint64 checkedAt);
    void retainProjects(const std::set<QString>& cacheKeys);
    void setBusy(bool busy);

   signals:
    void checkDue();

   private:
    friend class ManagedPackUpdateSchedulerTest;

    void onTimeout(qint64 now);
    void schedule();
    QString resultPath(const QString& cacheKey) const;

    std::map<QString, Result> m_results;
    std::set<QString> m_loadedKeys;
    QString m_cacheDirectory;
    QTimer m_timer;
    bool m_busy = false;
};
