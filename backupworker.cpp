#include "BackupWorker.h"
#include "SpeedAverager.h"

#include <QDirIterator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QThread>
#include <QElapsedTimer>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStorageInfo>

#include <utility>
#include <cmath>

BackupWorker::BackupWorker(Options opt, QObject* parent)
    : QObject(parent), m_opt(std::move(opt)) {}

static QString cleanRel(const QString& rel) {
    QString r = QDir::cleanPath(rel);
#ifdef Q_OS_WIN
    r.replace('\\', '/');
#endif
    return r;
}

// ---------- 命名空间 & 路径 ----------
static QString shortHash(const QString& s) {
    const QByteArray h = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(h.left(8));
}

QString BackupWorker::nsPrefix() const {
    if (!m_opt.nsName.isEmpty()) return m_opt.nsName;
    if (!m_cachedNs.isEmpty())   return m_cachedNs;
    const QString base = QFileInfo(m_opt.srcDir).completeBaseName(); // 目录名
    const QString abs  = QDir(m_opt.srcDir).absolutePath();
    m_cachedNs = base + "_" + shortHash(abs);
    return m_cachedNs;
}

QString BackupWorker::nsSubRoot() const {
    return QDir(m_opt.dstDir).absoluteFilePath(nsPrefix());
}

QString BackupWorker::dstAbsPath(const QString& rel) const {
    return QDir(nsSubRoot()).absoluteFilePath(cleanRel(rel));
}

QStringList BackupWorker::listAllFiles() const {
    QStringList out;
    QDirIterator it(m_opt.srcDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); out << QDir(m_opt.srcDir).relativeFilePath(it.filePath()); }
    out.sort(Qt::CaseInsensitive);
    for (QString& s : out) s = cleanRel(s);
    return out;
}

qint64 BackupWorker::calcTotalBytes() const {
    qint64 sum = 0;
    const QStringList rels = m_opt.filesWhitelist.isEmpty() ? listAllFiles() : m_opt.filesWhitelist;
    for (const QString& rel0 : rels) {
        const QString rel = cleanRel(rel0);
        if (shouldSkip(rel)) continue;
        QFileInfo fi(QDir(m_opt.srcDir).absoluteFilePath(rel));
        if (fi.exists() && fi.isFile()) sum += fi.size();
    }
    return sum;
}

bool BackupWorker::shouldSkip(const QString& rel) const {
    if (rel.isEmpty()) return true;
    if (!m_opt.ignoreGlobs.isEmpty()) {
        if (QDir::match(m_opt.ignoreGlobs, rel)) return true;
    }
    return false;
}

QByteArray BackupWorker::fileHashSha256(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    const qint64 BUF = 1 << 20;
    QByteArray buf; buf.resize(BUF);
    qint64 n;
    while ((n = f.read(buf.data(), BUF)) > 0) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        h.addData(QByteArrayView(buf.constData(), static_cast<qsizetype>(n)));
#else
        h.addData(buf.constData(), n);
#endif
    }
    return h.result();
}

bool BackupWorker::ensureDir(const QString& dirPath) {
    QDir d;
    return d.mkpath(dirPath);
}

bool BackupWorker::moveFileRobust(const QString& from, const QString& to) {
    if (QFile::exists(to)) QFile::remove(to);
    if (QFile::rename(from, to)) return true;
    if (QFile::copy(from, to)) { QFile::remove(from); return true; }
    return false;
}

QString BackupWorker::tsNow() {
    return QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
}

QString BackupWorker::metaRoot() const {
    return QDir(m_opt.dstDir).absoluteFilePath(".plugbackup_meta");
}
QString BackupWorker::versionsRoot() const {
    return QDir(metaRoot()).absoluteFilePath("versions");
}
QString BackupWorker::deletedRoot() const {
    return QDir(metaRoot()).absoluteFilePath("deleted");
}

QString BackupWorker::versionFilePath(const QString& rel0, const QString& ts) const {
    const QString rel = cleanRel(rel0);
    const QString baseDir = QFileInfo(rel).path();
    const QString name    = QFileInfo(rel).fileName();
    const QString dirPath = QDir(versionsRoot()).absoluteFilePath(nsPrefix() + "/" + baseDir);
    const QString fileOut = QString("%1.v%2").arg(name, ts);
    return QDir(dirPath).absoluteFilePath(fileOut);
}
QString BackupWorker::deletedFilePath(const QString& rel0, const QString& ts) const {
    const QString rel = cleanRel(rel0);
    const QString baseDir = QFileInfo(rel).path();
    const QString name    = QFileInfo(rel).fileName();
    const QString dirPath = QDir(deletedRoot()).absoluteFilePath(nsPrefix() + "/" + baseDir);
    const QString fileOut = QString("%1.d%2").arg(name, ts);
    return QDir(dirPath).absoluteFilePath(fileOut);
}

QString BackupWorker::writeMetaJson(const QString& payloadPath, const QString& rel,
                                    const QString& kind, const QString& ts) const {
    QJsonObject obj{
        {"kind", kind},
        {"ts",   ts},
        {"srcRoot", m_opt.srcDir},
        {"dstRoot", m_opt.dstDir},
        {"namespace", nsPrefix()},
        {"rel", rel},
        {"origAbs", QDir(m_opt.srcDir).absoluteFilePath(rel)},
        {"payload", payloadPath}
    };
    const QString metaPath = payloadPath + ".json";
    QFile f(metaPath);
    if (f.open(QIODevice::WriteOnly|QIODevice::Truncate)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.close();
    }
    return metaPath;
}

// ---------- 设备就绪/同一设备检测 ----------
bool BackupWorker::isDestReadySameDevice() const {
    QStorageInfo st(m_opt.dstDir);
    if (!st.isValid() || !st.isReady() || st.isReadOnly())
        return false;
    if (!m_expectedDevice.isEmpty() && st.device() != m_expectedDevice)
        return false; // 同一盘符/挂载点被其他设备接管
    return true;
}

void BackupWorker::waitUntilDestReadyOrStopped(const QString& phaseHint) {
    if (m_stop.loadAcquire()) return;

    if (!isDestReadySameDevice() && !m_offlineSignaled) {
        m_offlineSignaled = true;
        emit deviceOffline(phaseHint); // 提示：可能未插入/已弹出/卷标变化/只读/不同设备接管
        emit stateChanged(tr("设备离线/变更，等待中…%1").arg(
            phaseHint.isEmpty()?QString():QString(" · %1").arg(phaseHint)));
    }

    while (!isDestReadySameDevice() && !m_stop.loadAcquire()
           && !QThread::currentThread()->isInterruptionRequested()) {
        QThread::msleep(200); // 缩短等待粒度，加速响应停止
    }

    if (m_offlineSignaled && isDestReadySameDevice()) {
        m_offlineSignaled = false;
        emit deviceOnline();
        emit stateChanged(tr("设备已恢复，继续：%1").arg(
            phaseHint.isEmpty()?tr("任务"):phaseHint));
    }
}

// ---------- 快速相等判断 ----------
bool BackupWorker::likelySameByStat(const QString& srcAbs, const QString& dstAbs) const {
    QFileInfo s(srcAbs), d(dstAbs);
    if (!s.exists() || !d.exists() || !s.isFile() || !d.isFile()) return false;
    if (s.size() != d.size()) return false;
    // mtime 差值 ≤ 2 秒，认为“可能相同”，需要哈希确认；否则就当不同
    const qint64 diff = std::llabs(s.lastModified().toSecsSinceEpoch() - d.lastModified().toSecsSinceEpoch());
    return diff <= 2;
}

// ---------- 版本与删除留存 ----------
bool BackupWorker::maybeStashExistingVersion(const QString& rel0) {
    if (!m_opt.keepVersionsOnChange) return true;
    const QString rel = cleanRel(rel0);

    if (!isDestReadySameDevice()) return true; // 外层会 wait+retry

    const QString dstPath = dstAbsPath(rel);
    if (!QFileInfo::exists(dstPath)) return true;

    const QString srcPath = QDir(m_opt.srcDir).absoluteFilePath(rel);

    // 快速路径：只有“可能相同”时才哈希确认
    if (likelySameByStat(srcPath, dstPath)) {
        const QByteArray hashDst = fileHashSha256(dstPath);
        const QByteArray hashSrc = fileHashSha256(srcPath);
        if (!hashDst.isEmpty() && hashDst == hashSrc) return false; // 完全相同：不需要版本化&复制
    }

    const QString ts = tsNow();
    const QString outPath = versionFilePath(rel, ts);
    ensureDir(QFileInfo(outPath).absolutePath());
    if (!isDestReadySameDevice()) return true;

    if (moveFileRobust(dstPath, outPath)) {
        const QString meta = writeMetaJson(outPath, rel, "version", ts);
        emit versionCreated(rel, outPath, meta);
        return true;
    } else {
        // 空间不足/权限等 → 交由外层标记失败，不覆盖新内容
        return false;
    }
}

void BackupWorker::handleDeletions(const QSet<QString>& srcSet) {
    if (!m_opt.keepDeletedInVault) return;
    if (!isDestReadySameDevice()) { waitUntilDestReadyOrStopped(tr("处理删除项")); if (m_stop.loadAcquire()) return; }

    // 仅在该源的命名空间下清理
    const QString rootNs = nsSubRoot();
    if (!QDir(rootNs).exists()) return;

    QDirIterator it(rootNs, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && !m_stop.loadAcquire()) {
        it.next();
        const QString abs = it.filePath();

        // 排除元数据目录（位于 dst/.plugbackup_meta，下方遍历不到，这里只是防御）
        if (abs.contains("/.plugbackup_meta/") || abs.contains("\\.plugbackup_meta\\")) continue;

        if (!isDestReadySameDevice()) { waitUntilDestReadyOrStopped(tr("处理删除项")); if (m_stop.loadAcquire()) return; }

        // 计算相对路径（相对于 ns 子树）
        const QString relNs = cleanRel(QDir(rootNs).relativeFilePath(abs));
        const QString rel   = relNs; // 命名空间以外的“纯相对路径”

        if (srcSet.contains(rel)) continue; // 源还在 → 不算删除

        const QString ts = tsNow();
        const QString outPath = deletedFilePath(rel, ts);
        ensureDir(QFileInfo(outPath).absolutePath());
        if (!isDestReadySameDevice()) { waitUntilDestReadyOrStopped(tr("处理删除项")); if (m_stop.loadAcquire()) return; }

        if (moveFileRobust(abs, outPath)) {
            const QString meta = writeMetaJson(outPath, rel, "deleted", ts);
            emit deletedStashed(rel, outPath, meta);
        }
    }
}

void BackupWorker::sweepRetention() {
    const int days = qMax(0, m_opt.retentionDays);
    if (days == 0) return;
    if (!isDestReadySameDevice()) { waitUntilDestReadyOrStopped(tr("清理旧版本")); if (m_stop.loadAcquire()) return; }

    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-days);

    auto sweepDir = [&](const QString& root, const QString& marker){
        const QString base = QDir(root).absoluteFilePath(nsPrefix());
        if (!QDir(base).exists()) return;
        QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QString file = it.filePath();
            if (file.endsWith(".json", Qt::CaseInsensitive)) continue;
            const QString name = QFileInfo(file).fileName();
            const int pos = name.lastIndexOf(marker);
            if (pos < 0) continue;
            const QString tsStr = name.mid(pos+2);
            QDateTime ts = QDateTime::fromString(tsStr, "yyyyMMdd-HHmmss");
            ts.setTimeSpec(Qt::UTC);
            if (!ts.isValid()) continue;
            if (ts < cutoff) {
                QFile::remove(file);
                QFile::remove(file + ".json");
            }
        }
    };
    sweepDir(versionsRoot(), ".v");
    sweepDir(deletedRoot(),  ".d");
}

// ---------- 主流程 ----------
void BackupWorker::run() {
    // 记录期望设备指纹（首次就绪时）
    {
        QStorageInfo st(m_opt.dstDir);
        if (st.isValid() && st.isReady())
            m_expectedDevice = st.device();
        else
            m_expectedDevice.clear();
    }

    // 若启动即离线，等待
    waitUntilDestReadyOrStopped(tr("启动"));
    if (m_stop.loadAcquire()) { emit finished(false, tr("已取消")); return; }

    emit stateChanged(QObject::tr("扫描中"));
    const QStringList relsAll = m_opt.filesWhitelist.isEmpty() ? listAllFiles() : m_opt.filesWhitelist;

    // 源文件集合（用于删除处理）
    QSet<QString> srcSet;
    for (const QString& r : relsAll) {
        const QString rel = cleanRel(r);
        if (!shouldSkip(rel)) srcSet.insert(rel);
    }

    m_totalBytes = 0;
    for (const QString& rel : srcSet) {
        QFileInfo fiSrc(QDir(m_opt.srcDir).absoluteFilePath(rel));
        if (fiSrc.exists() && fiSrc.isFile()) m_totalBytes += fiSrc.size();
    }
    emit progressUpdated(0, m_totalBytes);

    qint64 bytesDone = 0;
    bool allOk = true;
    SpeedAverager speed(5000);
    QElapsedTimer ticker; ticker.start();

    emit stateChanged(QObject::tr("复制中"));

    for (const QString& rel : srcSet) {
        if (m_stop.loadAcquire()) break;
        while (m_pause.loadAcquire() && !m_stop.loadAcquire()) QThread::msleep(50);

        const QString srcPath = QDir(m_opt.srcDir).absoluteFilePath(rel);
        QFileInfo fiSrc(srcPath);
        if (!fiSrc.exists() || !fiSrc.isFile()) continue;

        emit fileStarted(rel, fiSrc.size());

        // 设备就绪保障
        waitUntilDestReadyOrStopped(tr("准备复制"));
        if (m_stop.loadAcquire()) break;

        // 若目标存在，先做“版本化或跳过”
        if (QFileInfo::exists(dstAbsPath(rel))) {
            bool r = maybeStashExistingVersion(rel);
            if (!isDestReadySameDevice()) { // 期间设备变更 → 重来
                waitUntilDestReadyOrStopped(tr("版本化"));
                if (m_stop.loadAcquire()) break;
                r = maybeStashExistingVersion(rel);
            }
            if (!r) { // 版本化失败，标记文件失败并跳过复制
                emit fileFinished(rel, false, QObject::tr("版本归档失败"));
                allOk = false;
                continue;
            }
            // 若返回 false 表示“完全相同”→ 已在 maybeStashExistingVersion 中处理为“跳过复制”
            // 但我们这里为清晰起见再检测一次：如果目标仍在且与源完全一致，就跳过并累加进度
            const QString dstPath = dstAbsPath(rel);
            if (QFileInfo::exists(dstPath) && likelySameByStat(srcPath, dstPath)) {
                const QByteArray a = fileHashSha256(srcPath);
                const QByteArray b = fileHashSha256(dstPath);
                if (!a.isEmpty() && a == b) {
                    bytesDone += fiSrc.size();
                    emit fileFinished(rel, true, QString());
                    // 刷新速率与进度（节流）
                    speed.onProgress(bytesDone);
                    if (ticker.elapsed() > 200) {
                        const double bps = speed.avgBytesPerSec();
                        emit speedUpdated(bps);
                        const qint64 remain = m_totalBytes - bytesDone;
                        const qint64 eta = bps > 1.0 ? qint64(remain / bps) : -1;
                        emit etaUpdated(eta);
                        emit progressUpdated(bytesDone, m_totalBytes);
                        ticker.restart();
                    }
                    continue;
                }
            }
        }

        // 复制 + 离线自动等待重试
        for (;;) {
            if (m_stop.loadAcquire()) break;
            while (m_pause.loadAcquire() && !m_stop.loadAcquire()) QThread::msleep(50);

            if (!isDestReadySameDevice()) {
                waitUntilDestReadyOrStopped(tr("复制"));
                if (m_stop.loadAcquire()) break;
                continue; // 设备恢复后再试
            }

            bool ok = copyOneFile(rel, &bytesDone);
            if (!ok) {
                if (!isDestReadySameDevice()) {
                    // 复制过程中设备掉线：等待、再试
                    waitUntilDestReadyOrStopped(tr("复制重试"));
                    if (m_stop.loadAcquire()) break;
                    continue;
                }
                emit fileFinished(rel, false, QObject::tr("复制失败"));
                allOk = false;
                break;
            }

            if (m_opt.verifyAfterWrite) {
                emit stateChanged(QObject::tr("校验中 · %1").arg(rel));
                bool vok = verifyFile(rel);
                if (!vok) {
                    if (!isDestReadySameDevice()) {
                        waitUntilDestReadyOrStopped(tr("校验重试"));
                        if (m_stop.loadAcquire()) break;
                        continue; // 回到 copy 再来一遍最稳妥
                    }
                    emit fileFinished(rel, false, QObject::tr("校验失败"));
                    allOk = false;
                    break;
                }
            }

            // 成功
            emit fileFinished(rel, true, QString());
            break;
        }

        // 速率/ETA 更新（节流）
        speed.onProgress(bytesDone);
        if (ticker.elapsed() > 200) {
            const double bps = speed.avgBytesPerSec();
            emit speedUpdated(bps);
            const qint64 remain = m_totalBytes - bytesDone;
            const qint64 eta = bps > 1.0 ? qint64(remain / bps) : -1;
            emit etaUpdated(eta);
            emit progressUpdated(bytesDone, m_totalBytes);
            ticker.restart();
        }
    }

    // 删除处理
    if (!m_stop.loadAcquire()) {
        waitUntilDestReadyOrStopped(tr("处理删除项"));
        if (!m_stop.loadAcquire()) handleDeletions(srcSet);
    }

    // 清理保留期
    if (!m_stop.loadAcquire()) {
        waitUntilDestReadyOrStopped(tr("清理旧版本"));
        if (!m_stop.loadAcquire()) sweepRetention();
    }

    emit progressUpdated(m_totalBytes, m_totalBytes);
    emit finished(allOk, allOk ? QObject::tr("完成") : QObject::tr("部分失败"));
}

bool BackupWorker::copyOneFile(const QString& rel0, qint64* bytesDone) {
    const QString rel = cleanRel(rel0);
    const QString srcPath = QDir(m_opt.srcDir).absoluteFilePath(rel);
    const QString dstPath = dstAbsPath(rel);

    if (!isDestReadySameDevice()) return false;

    if (!ensureDir(QFileInfo(dstPath).absolutePath())) return false;
    if (!isDestReadySameDevice()) return false;

    QFile in(srcPath);
    if (!in.open(QIODevice::ReadOnly)) return false;

    QFile out(dstPath + ".part");
    if (!out.open(QIODevice::WriteOnly)) {
        in.close();
        return false;
    }

    const qint64 BUF = 1 << 20; // 1MB
    QByteArray buf; buf.resize(BUF);
    qint64 n;
    QElapsedTimer t; t.start();
    qint64 sentInWindow = 0;
    const int windowMs = 100;

    while ((n = in.read(buf.data(), BUF)) > 0) {
        if (m_stop.loadAcquire() || QThread::currentThread()->isInterruptionRequested()) {
            out.close(); QFile::remove(dstPath + ".part"); in.close();
            return false;
        }
        while (m_pause.loadAcquire() && !m_stop.loadAcquire()
               && !QThread::currentThread()->isInterruptionRequested()) {
            QThread::msleep(50);
        }

        if (!isDestReadySameDevice()) {
            // 目标掉线：清理临时文件，交由上层等待并重试
            out.close();
            QFile::remove(dstPath + ".part");
            in.close();
            return false;
        }

        if (m_opt.speedLimitBps > 0) {
            const qint64 budgetWindow = (m_opt.speedLimitBps * windowMs) / 1000;
            if (sentInWindow + n > budgetWindow) {
                const int elapsed = int(t.elapsed());
                if (elapsed < windowMs) QThread::msleep(windowMs - elapsed);
                t.restart();
                sentInWindow = 0;
            }
        }

        qint64 w = out.write(buf.constData(), n);
        if (w != n) {
            out.close();
            QFile::remove(dstPath + ".part");
            in.close();
            return false;
        }
        *bytesDone += n;
        if (m_opt.speedLimitBps > 0) sentInWindow += n;
    }

    out.flush(); out.close(); in.close();

    // 原子替换
    QFile::remove(dstPath);
    if (!QFile::rename(dstPath + ".part", dstPath)) {
        QFile::remove(dstPath + ".part");
        return false;
    }

    // 可选：把目标 mtime 调整为源 mtime（更友好）
    QFileInfo fiSrc(srcPath);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QFile dst(dstPath);
    if (dst.open(QIODevice::ReadWrite)) {
        dst.setFileTime(fiSrc.lastModified(), QFileDevice::FileModificationTime);
        dst.close();
    }
#endif
    return true;
}

bool BackupWorker::verifyFile(const QString& rel0) {
    const QString rel = cleanRel(rel0);
    const QString srcPath = QDir(m_opt.srcDir).absoluteFilePath(rel);
    const QString dstPath = dstAbsPath(rel);

    if (!isDestReadySameDevice()) return false;

    QByteArray a = fileHashSha256(srcPath);
    QByteArray b = fileHashSha256(dstPath);
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a == b) return true;

    int delay = 1000;
    for (int i = 0; i < m_opt.maxRetries; ++i) {
        QThread::msleep(delay);
        if (!isDestReadySameDevice()) return false;
        b = fileHashSha256(dstPath);
        if (!b.isEmpty() && a == b) return true;
        delay = qMin(delay * 2, 30000);
    }
    return false;
}

void BackupWorker::requestPause(bool p) { m_pause.storeRelease(p ? 1 : 0); }
void BackupWorker::requestStop()       { m_stop.storeRelease(1); }
