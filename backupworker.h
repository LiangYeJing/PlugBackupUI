#pragma once
#include <QObject>
#include <QAtomicInt>
#include <QString>
#include <QStringList>
#include <QByteArray>

/**
 * 单个“源目录 → 目标目录”的备份任务
 * - 命名空间隔离：dst/<nsPrefix()>/rel/path，避免多源同名覆盖
 * - 快速校验：size/mtime 快速判断，仅在可能相同的情况下才哈希
 * - 写后校验（可配）、失败重试、限速、忽略/白名单
 * - 历史版本与删除留存（带保留天数）
 * - 安全：目标设备指纹校验；离线等待；发离线/恢复信号；绝不误写
 */
class BackupWorker : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString srcDir;                  // 源目录
        QString dstDir;                  // 目标目录（在其下：<ns>/...）
        bool    verifyAfterWrite = true;
        int     maxRetries       = 3;
        QStringList ignoreGlobs;         // 忽略（glob）
        QStringList filesWhitelist;      // 相对路径白名单，空=全量
        qint64  speedLimitBps    = 0;    // 限速 B/s（0 不限）

        // 版本/删除留存
        bool    keepVersionsOnChange = true;
        bool    keepDeletedInVault   = true;
        int     retentionDays        = 7;

        // 可选：自定义命名空间名（留空则自动生成）
        QString nsName;
    };

    explicit BackupWorker(Options opt, QObject* parent=nullptr);

public slots:
    void run();                 // 放入 QThread 后开始
    void requestPause(bool p);  // 暂停/继续
    void requestStop();         // 取消

signals:
    // 任务级
    void progressUpdated(qint64 bytesDone, qint64 bytesTotal);
    void speedUpdated(double bytesPerSec);
    void etaUpdated(qint64 secondsLeft);
    void stateChanged(const QString& stateText);
    void finished(bool ok, const QString& summary);

    // 文件级
    void fileStarted(const QString& relPath, qint64 size);
    void fileFinished(const QString& relPath, bool ok, const QString& err);

    // 版本/删除留存
    void versionCreated(const QString& rel, const QString& versionFilePath, const QString& metaPath);
    void deletedStashed(const QString& rel, const QString& deletedFilePath, const QString& metaPath);

    // 设备事件（状态切换才发一次）
    void deviceOffline(const QString& phaseHint);
    void deviceOnline();

private:
    // 主流程
    qint64 calcTotalBytes() const;
    QStringList listAllFiles() const;
    bool shouldSkip(const QString& rel) const;
    bool copyOneFile(const QString& rel, qint64* bytesDone); // .part→rename
    bool verifyFile(const QString& rel);

    // 版本与删除留存
    bool maybeStashExistingVersion(const QString& rel);
    void handleDeletions(const QSet<QString>& srcSet);
    void sweepRetention();

    // 安全：目标设备就绪/同一设备检测 + 等待
    bool isDestReadySameDevice() const;
    void waitUntilDestReadyOrStopped(const QString& phaseHint = QString());

    // 辅助
    static QByteArray fileHashSha256(const QString& path);
    static bool ensureDir(const QString& dirPath);
    static bool moveFileRobust(const QString& from, const QString& to);
    static QString tsNow();

    // 命名空间 & 路径
    QString nsPrefix() const;                              // e.g. "Photos_7a1c3bde"
    QString nsSubRoot() const;                             // dst/<ns>/
    QString dstAbsPath(const QString& rel) const;          // dst/<ns>/<rel>
    QString metaRoot() const;                              // dst/.plugbackup_meta
    QString versionsRoot() const;                          // dst/.plugbackup_meta/versions
    QString deletedRoot() const;                           // dst/.plugbackup_meta/deleted
    QString versionFilePath(const QString& rel, const QString& ts) const; // versions/<ns>/<rel>.vTS
    QString deletedFilePath(const QString& rel, const QString& ts) const; // deleted/<ns>/<rel>.dTS
    QString writeMetaJson(const QString& payloadPath, const QString& rel,
                          const QString& kind, const QString& ts) const;

    // 快速相等判断（减少哈希开销）
    bool likelySameByStat(const QString& srcAbs, const QString& dstAbs) const;

private:
    Options    m_opt;
    QAtomicInt m_pause{0}, m_stop{0};
    qint64     m_totalBytes = 0;

    // 设备指纹：首次 run() 记录
    QByteArray m_expectedDevice;

    // 防抖：离线提示仅一次
    bool       m_offlineSignaled = false;

    // 缓存自动生成的 ns
    mutable QString m_cachedNs;
};
