#pragma once
#include <QMainWindow>
#include <QVector>
#include <QSet>
#include <QMap>
#include <QCloseEvent>
#include <QPointer>

class QListWidget;
class QLineEdit;
class QPushButton;
class QLabel;
class QListWidgetItem;
class QTableWidget;
class QCheckBox;
class QSpinBox;
class QFileSystemWatcher;
class QTimer;
class QToolButton;

class BackupWorker;

/**
 * @brief 主窗口：源/目标选择 + 自动化策略 + 任务表（暂停/继续/取消）
 *        扩展：失败重试、忽略规则、递归监听、版本与删除留存（.plugbackup_meta）、一键恢复
 *        新增：保留天数、限速可配置；智能模式（系统繁忙自动暂停、空闲自动恢复）
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent=nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    // —— 选择界面 —— //
    void onAddSource();
    void onRemoveSelected();
    void onClearSources();
    void onChooseDest();
    void onValidate();

    // —— 任务控制 —— //
    void onStartBackup();
    void onPauseRow();
    void onResumeRow();
    void onCancelRow();
    void onRetryFailed();

    // —— 自动化 —— //
    void onAutoOptionsChanged();
    void onDeviceCheckTick();
    void onAutoIntervalTick();
    void onStabilityTick();
    void onWatchedPathChanged(const QString& path);
    void onWorkerDeviceOffline(const QString& phase);
    void onWorkerDeviceOnline();

    // —— 版本/回收站 —— //
    void onScanVault();
    void onRestoreSelectedVersion();
    void onRestoreSelectedDeleted();

    // —— 智能模式 —— //
    void onSmartTick();

private:
    // —— 工具方法 —— //
    bool isWritableDir(const QString& path) const;
    bool isSubPath(const QString& parent, const QString& child) const;
    bool anySourceInsideAnother(QString *a=nullptr, QString *b=nullptr) const;
    bool isDestOnline() const;

    // 监听（递归）
    void refreshWatcher();
    void rebuildRecursiveWatch(const QString& root);

    // 自动触发备份
    void tryStartAutoBackup(const QString& reason);

    // 忽略规则解析
    QStringList splitPatterns(const QString& text) const;

    // 设置持久化
    void loadSettings();
    void saveSettings() const;

    // 任务表工具
    int  addJobRow(const QString& src, const QString& dst);
    void addOpButtons(int row);
    void updateGlobalStats();
    QSet<const void*> m_offlineWorkers; // 正处于离线等待的 worker 集合（用指针去重）
    QPointer<class QMessageBox> m_offlineBox; // 非模态提示框

    // 线程收尾
    void stopAllTasks(int waitMs = 15000);

    // 版本/回收站 面板工具
    QString metaRootOfDest() const;
    void populateVaultLists();
    static QString itemPayloadPath(QListWidgetItem* it);
    static QString itemMetaPath(QListWidgetItem* it);

    // 智能模式：批量暂停/恢复
    void pauseAllTasks(bool fromSmart);
    void resumeAllTasks(bool fromSmart);

    // 智能模式：CPU 利用率采样
    double sampleSystemCpuUsagePercent();

    // 页面美化
    void applyTheme();   // 设置全局 QSS 主题
    void polishUi();     // 细化布局、表格和按钮图标等

private:
    // ======= UI：选择区 ======= //
    QListWidget* m_sourceList = nullptr;
    QLineEdit*   m_destEdit   = nullptr;
    QLineEdit*   m_ignoreEdit = nullptr; // 忽略规则（; 分隔，glob）
    QPushButton* m_btnStart   = nullptr;
    QLabel*      m_statusLbl  = nullptr;

    // ======= UI：任务表 ======= //
    QTableWidget* m_jobs       = nullptr;
    QLabel*       m_totalSpeed = nullptr;
    QLabel*       m_totalEta   = nullptr;
    QVector<double> m_rowSpeedBps;
    QVector<qint64> m_rowRemainBytes;

    // ======= UI：失败重试 ======= //
    QListWidget* m_failedList  = nullptr;
    QPushButton* m_btnRetryFailed = nullptr;

    // ======= UI：版本与删除留存 ======= //
    QListWidget* m_versionsList = nullptr;
    QListWidget* m_deletedList  = nullptr;
    QPushButton* m_btnScanVault = nullptr;
    QPushButton* m_btnRestoreVersion = nullptr;
    QPushButton* m_btnRestoreDeleted = nullptr;

    // ======= 自动化选项 ======= //
    QCheckBox* m_chkAutoInterval = nullptr;
    QSpinBox*  m_spinIntervalMin  = nullptr;
    QCheckBox* m_chkAutoOnClose   = nullptr;
    QSpinBox*  m_spinStabSec      = nullptr;
    QSpinBox*  m_spinDevChkMin    = nullptr;

    // ======= 高级选项（新增） ======= //
    QSpinBox*  m_spinRetentionDays = nullptr; // 保留天数（版本/删除留存）
    QSpinBox*  m_spinSpeedLimitMB  = nullptr; // 限速（MB/s，0=不限）
    QCheckBox* m_chkSmart          = nullptr; // 智能模式：繁忙时暂停
    QSpinBox*  m_spinSmartCpuHi    = nullptr; // 繁忙阈值（%）
    QSpinBox*  m_spinSmartPollSec  = nullptr; // 轮询间隔（秒）

    // ======= 监控与定时 ======= //
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer* m_timerDevice  = nullptr;
    QTimer* m_timerInterval= nullptr;
    QTimer* m_timerStab    = nullptr;
    QTimer* m_timerSmart   = nullptr;   // 智能模式轮询

    bool   m_deviceOnline   = false;
    bool   m_backupRunning  = false;
    bool   m_pendingChanges = false;
    qint64 m_lastChangeMs   = 0;

    // 智能模式状态
    bool   m_smartPaused    = false;
    int    m_smartBusyCount = 0; // 连续繁忙计数
    int    m_smartIdleCount = 0; // 连续空闲计数

    // CPU 采样（Windows）
    quint64 m_prevIdle      = 0;
    quint64 m_prevKernel    = 0;
    quint64 m_prevUser      = 0;

    // 已递归添加的目录集合（绝对路径）
    QSet<QString> m_watchedDirs;

    // 行 → 任务对象
    struct Task {
        QString src;
        QString dst;
        BackupWorker* worker = nullptr;
        QThread*      thread = nullptr;
        int           row    = -1;
        bool          paused = false;
    };
    QVector<Task> m_tasks;

    // 失败文件：key=srcDir，value=相对路径列表
    QMap<QString, QStringList> m_failedBySrc;
};
