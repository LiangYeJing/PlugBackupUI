#include "MainWindow.h"
#include "BackupWorker.h"
#include "SpeedAverager.h"

#include <QScrollArea>
#include <QComboBox>
#include <QElapsedTimer>
#include <QGraphicsDropShadowEffect>
#include <QStyle>
#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QStatusBar>
#include <QMessageBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QProgressBar>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QCheckBox>
#include <QSpinBox>
#include <QFileSystemWatcher>
#include <QStorageInfo>
#include <QToolButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#endif

static QString humanEta(qint64 sec) {
    if (sec < 0) return QStringLiteral("--:--:--");
    const int h = int(sec/3600), m = int((sec%3600)/60), s = int(sec%60);
    return QString::asprintf("%02d:%02d:%02d", h, m, s);
}
static QString humanSpeed(double bps) {
    if (bps <= 0) return QStringLiteral("0 MB/s");
    const double mbps = bps / (1024.0*1024.0);
    return QString::asprintf("%.2f MB/s", mbps);
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("PlugBackup — 自动备份"));

    // auto *central = new QWidget(this);
    // auto *vbox = new QVBoxLayout(central);

    // page 承载你现有的全部布局内容
    auto *page = new QWidget(this);
    auto *vbox = new QVBoxLayout(page);

    // —— 顶部总览 —— //
    {
        auto *row = new QHBoxLayout();
        m_totalSpeed = new QLabel(tr("总速率：0 MB/s"), page);
        m_totalEta   = new QLabel(tr("剩余时间：--:--:--"), page);
        row->addWidget(m_totalSpeed);
        row->addSpacing(20);
        row->addWidget(m_totalEta);
        row->addStretch(1);
        vbox->addLayout(row);
    }

    // —— 源目录列表 —— //
    vbox->addWidget(new QLabel(tr("源目录列表"), page));
    m_sourceList = new QListWidget(page);
    m_sourceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    vbox->addWidget(m_sourceList, 1);

    // —— 源目录按钮 —— //
    {
        auto *row = new QHBoxLayout();
        auto *btnAdd   = new QPushButton(tr("添加"), page);
        auto *btnDel   = new QPushButton(tr("删除所选"), page);
        auto *btnClear = new QPushButton(tr("清空"), page);
        row->addWidget(btnAdd);
        row->addWidget(btnDel);
        row->addWidget(btnClear);
        row->addStretch(1);
        vbox->addLayout(row);

        connect(btnAdd,   &QPushButton::clicked, this, &MainWindow::onAddSource);
        connect(btnDel,   &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
        connect(btnClear, &QPushButton::clicked, this, &MainWindow::onClearSources);
    }

    // —— 目标目录 + 忽略规则 —— //
    {
        auto *g = new QGridLayout();
        auto *lblDest = new QLabel(tr("备份目标目录（移动硬盘上的文件夹）"), page);
        m_destEdit = new QLineEdit(page);
        m_destEdit->setReadOnly(true);
        auto *btnChooseDest = new QPushButton(tr("选择目标文件夹…"), page);

        g->addWidget(lblDest,     0,0);
        g->addWidget(m_destEdit,  0,1);
        g->addWidget(btnChooseDest,0,2);
        g->setColumnStretch(1, 1);

        g->addWidget(new QLabel(tr("忽略规则（; 分隔，支持通配符）"), page), 1,0);
        m_ignoreEdit = new QLineEdit(page);
        m_ignoreEdit->setPlaceholderText(tr("例如：*.tmp; node_modules/*; *.log"));
        g->addWidget(m_ignoreEdit, 1,1,1,2);

        vbox->addLayout(g);

        connect(btnChooseDest, &QPushButton::clicked, this, &MainWindow::onChooseDest);
        connect(m_ignoreEdit,  &QLineEdit::textChanged, this, &MainWindow::onAutoOptionsChanged);
    }

    // —— 自动化策略 —— //
    {
        auto *box = new QGroupBox(tr("自动化策略"), page);
        auto *g = new QGridLayout(box);

        m_chkAutoInterval = new QCheckBox(tr("按间隔自动备份"), box);
        m_spinIntervalMin = new QSpinBox(box);
        m_spinIntervalMin->setRange(1, 24*60);
        m_spinIntervalMin->setValue(30);

        m_chkAutoOnClose = new QCheckBox(tr("文件“正常关闭”（稳定窗口）后自动备份"), box);
        m_spinStabSec    = new QSpinBox(box);
        m_spinStabSec->setRange(5, 3600);
        m_spinStabSec->setValue(120);

        auto *lblDevChk  = new QLabel(tr("设备在线检测（分钟）"), box);
        m_spinDevChkMin  = new QSpinBox(box);
        m_spinDevChkMin->setRange(1, 24*60);
        m_spinDevChkMin->setValue(60);

        int r=0;
        g->addWidget(m_chkAutoInterval, r,0);
        g->addWidget(new QLabel(tr("间隔（分钟）"), box), r,1);
        g->addWidget(m_spinIntervalMin, r,2); ++r;
        g->addWidget(m_chkAutoOnClose, r,0);
        g->addWidget(new QLabel(tr("稳定窗口（秒）"), box), r,1);
        g->addWidget(m_spinStabSec, r,2); ++r;
        g->addWidget(lblDevChk, r,0);
        g->addWidget(m_spinDevChkMin, r,1);

        vbox->addWidget(box);

        connect(m_chkAutoInterval, &QCheckBox::toggled, this, &MainWindow::onAutoOptionsChanged);
        connect(m_chkAutoOnClose,  &QCheckBox::toggled, this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinIntervalMin, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinStabSec,     qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinDevChkMin,   qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
    }

    // —— 高级设置（新增：保留天数、限速、智能模式） —— //
    {
        auto *box = new QGroupBox(tr("高级设置"), page);
        auto *g = new QGridLayout(box);

        // 保留天数
        g->addWidget(new QLabel(tr("保留天数（版本/删除留存）"), box), 0,0);
        m_spinRetentionDays = new QSpinBox(box);
        m_spinRetentionDays->setRange(0, 3650); // 0=不保留
        m_spinRetentionDays->setValue(7);
        g->addWidget(m_spinRetentionDays, 0,1);

        // 限速
        g->addWidget(new QLabel(tr("限速（MB/s，0=不限）"), box), 0,2);
        m_spinSpeedLimitMB = new QSpinBox(box);
        m_spinSpeedLimitMB->setRange(0, 4096);
        m_spinSpeedLimitMB->setValue(0);
        g->addWidget(m_spinSpeedLimitMB, 0,3);

        // 智能模式
        m_chkSmart = new QCheckBox(tr("智能模式：系统繁忙时自动暂停，空闲时自动恢复"), box);
        g->addWidget(m_chkSmart, 1,0,1,4);

        g->addWidget(new QLabel(tr("繁忙阈值CPU(%)"), box), 2,0);
        m_spinSmartCpuHi = new QSpinBox(box);
        m_spinSmartCpuHi->setRange(20, 100);
        m_spinSmartCpuHi->setValue(65);
        g->addWidget(m_spinSmartCpuHi, 2,1);

        g->addWidget(new QLabel(tr("轮询间隔（秒）"), box), 2,2);
        m_spinSmartPollSec = new QSpinBox(box);
        m_spinSmartPollSec->setRange(2, 60);
        m_spinSmartPollSec->setValue(5);
        g->addWidget(m_spinSmartPollSec, 2,3);

        vbox->addWidget(box);

        connect(m_chkSmart,        &QCheckBox::toggled, this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinSmartCpuHi,  qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinSmartPollSec,qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinRetentionDays,qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
        connect(m_spinSpeedLimitMB,qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::onAutoOptionsChanged);
    }

    // —— 任务表（增加“操作”列） —— //
    m_jobs = new QTableWidget(0, 7, page);
    m_jobs->setHorizontalHeaderLabels({tr("源路径"), tr("目标路径"), tr("进度"), tr("速率"), tr("剩余时间"), tr("状态"), tr("操作")});
    m_jobs->horizontalHeader()->setStretchLastSection(true);
    m_jobs->verticalHeader()->setVisible(false);
    m_jobs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vbox->addWidget(m_jobs, 2);

    // —— 失败面板 —— //
    {
        auto *row = new QHBoxLayout();
        m_failedList = new QListWidget(page);
        m_failedList->setMinimumHeight(80);
        m_btnRetryFailed = new QPushButton(tr("只重试失败"), page);
        row->addWidget(new QLabel(tr("失败文件："), page));
        row->addWidget(m_failedList, 1);
        row->addWidget(m_btnRetryFailed);
        vbox->addLayout(row);
        connect(m_btnRetryFailed, &QPushButton::clicked, this, &MainWindow::onRetryFailed);
    }

    // —— 版本 / 回收站 —— //
    {
        auto *box = new QGroupBox(tr("历史版本 与 删除留存（来自 .plugbackup_meta）"), page);
        auto *g = new QGridLayout(box);

        m_versionsList = new QListWidget(box);
        m_deletedList  = new QListWidget(box);
        m_versionsList->setMinimumHeight(120);
        m_deletedList->setMinimumHeight(120);

        m_btnScanVault       = new QPushButton(tr("扫描"), box);
        m_btnRestoreVersion  = new QPushButton(tr("恢复历史版本到源"), box);
        m_btnRestoreDeleted  = new QPushButton(tr("恢复删除留存到源"), box);

        int r=0;
        g->addWidget(new QLabel(tr("历史版本"), box), r,0);
        g->addWidget(m_btnScanVault, r,1); ++r;
        g->addWidget(m_versionsList, r,0,1,2); ++r;
        g->addWidget(new QLabel(tr("删除留存"), box), r,0); ++r;
        g->addWidget(m_deletedList, r,0,1,2); ++r;

        auto *row = new QHBoxLayout();
        row->addStretch(1);
        row->addWidget(m_btnRestoreVersion);
        row->addWidget(m_btnRestoreDeleted);
        g->addLayout(row, r,0,1,2);

        vbox->addWidget(box);

        connect(m_btnScanVault,      &QPushButton::clicked, this, &MainWindow::onScanVault);
        connect(m_btnRestoreVersion, &QPushButton::clicked, this, &MainWindow::onRestoreSelectedVersion);
        connect(m_btnRestoreDeleted, &QPushButton::clicked, this, &MainWindow::onRestoreSelectedDeleted);
    }

    // —— 操作区 —— //
    {
        auto *row = new QHBoxLayout();
        row->addStretch(1);
        m_btnStart = new QPushButton(tr("开始备份"), page);
        m_btnStart->setEnabled(false);
        row->addWidget(m_btnStart);
        vbox->addLayout(row);
        connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStartBackup);
    }

    // setCentralWidget(central);
    // 用滚动容器包裹整个页面
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setCentralWidget(scroll);

    // 让页面有合适的最小可视区域，但不“固定”大小
    setMinimumSize(960, 640);  // 可按需调整，支持任意拉伸
    page->layout()->setContentsMargins(12,12,12,12);
    page->layout()->setSpacing(10);


    // —— 状态栏 —— //
    m_statusLbl = new QLabel(this);
    statusBar()->addWidget(m_statusLbl, 1);

    // —— 列表变化校验 —— //
    connect(m_sourceList, &QListWidget::itemChanged,          this, &MainWindow::onValidate);
    connect(m_sourceList, &QListWidget::itemSelectionChanged, this, &MainWindow::onValidate);

    // —— 监控 + 定时器 —— //
    m_watcher      = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &MainWindow::onWatchedPathChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,      this, &MainWindow::onWatchedPathChanged);

    m_timerDevice  = new QTimer(this);
    connect(m_timerDevice, &QTimer::timeout, this, &MainWindow::onDeviceCheckTick);

    m_timerInterval= new QTimer(this);
    connect(m_timerInterval, &QTimer::timeout, this, &MainWindow::onAutoIntervalTick);

    m_timerStab    = new QTimer(this);
    m_timerStab->setSingleShot(true);
    connect(m_timerStab, &QTimer::timeout, this, &MainWindow::onStabilityTick);

    m_timerSmart   = new QTimer(this);
    connect(m_timerSmart, &QTimer::timeout, this, &MainWindow::onSmartTick);

    // 统一留白
    page->layout()->setContentsMargins(12,12,12,12);
    page->layout()->setSpacing(10);

    // 应用主题与细化 UI
    applyTheme();
    polishUi();

    // 让大多数字段在水平方向可拉伸
    if (m_destEdit)       m_destEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (m_ignoreEdit)     m_ignoreEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (m_sourceList)     m_sourceList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (m_failedList)     m_failedList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (m_versionsList)   m_versionsList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (m_deletedList)    m_deletedList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (m_jobs)           m_jobs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 长文字标签自动换行，避免被压到一行
    for (QLabel* lab : findChildren<QLabel*>()) {
        lab->setWordWrap(true);
    }

    // 表格默认留点高度，不至于把下面面板“顶没”
    if (m_jobs) {
        m_jobs->setMinimumHeight(220);     // 你也可以设更高/更低
        m_jobs->horizontalHeader()->setStretchLastSection(true);
    }

    // 让数值框、下拉框别把行高拉太高
    for (QSpinBox* sp : findChildren<QSpinBox*>()) {
        sp->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }
    for (QComboBox* cb : findChildren<QComboBox*>()) {
        cb->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }


    // —— 初始化 —— //
    loadSettings();
    onValidate();
    onAutoOptionsChanged();
    onDeviceCheckTick();

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]{ stopAllTasks(15000); });
}

MainWindow::~MainWindow() {
    saveSettings();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    bool anyRunning = false;
    for (const auto &t : m_tasks) {
        if (t.thread && t.thread->isRunning()) { anyRunning = true; break; }
    }
    if (anyRunning) {
        statusBar()->showMessage(tr("正在停止后台任务…"), 2000);
        stopAllTasks(15000);
    }
    e->accept();
}

// ========== 源/目标选择 ==========
void MainWindow::onAddSource() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择源文件夹"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) return;
    const QString clean = QDir::cleanPath(dir);
    for (int i=0; i<m_sourceList->count(); ++i) {
        if (QDir::cleanPath(m_sourceList->item(i)->text()) == clean) {
            statusBar()->showMessage(tr("该目录已在列表中：%1").arg(clean), 3000);
            return;
        }
    }
    auto *item = new QListWidgetItem(clean);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(Qt::Checked);
    m_sourceList->addItem(item);
    refreshWatcher();
    onValidate();
}
void MainWindow::onRemoveSelected() { qDeleteAll(m_sourceList->selectedItems()); refreshWatcher(); onValidate(); }
void MainWindow::onClearSources()   { m_sourceList->clear(); refreshWatcher(); onValidate(); }
void MainWindow::onChooseDest() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择备份目标文件夹（移动硬盘上的目录）"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) return;
    m_destEdit->setText(QDir::cleanPath(dir));
    onValidate();
    onDeviceCheckTick();
}

// ========== 校验 ==========
bool MainWindow::isWritableDir(const QString &path) const {
    if (path.isEmpty()) return false;
    QDir d(path);
    if (!d.exists()) return false;
    QFileInfo info(path);
    return info.isDir() && info.isWritable();
}
bool MainWindow::isSubPath(const QString &parent, const QString &child) const {
    if (parent.isEmpty() || child.isEmpty()) return false;
    const QString p = QDir(parent).absolutePath() + QDir::separator();
    const QString c = QDir(child).absolutePath();
    return c.startsWith(p, Qt::CaseInsensitive);
}
bool MainWindow::anySourceInsideAnother(QString *a, QString *b) const {
    QStringList srcs;
    for (int i=0; i<m_sourceList->count(); ++i) srcs << QDir::cleanPath(m_sourceList->item(i)->text());
    for (int i=0; i<srcs.size(); ++i) for (int j=0; j<srcs.size(); ++j) {
            if (i==j) continue;
            if (isSubPath(srcs[i], srcs[j])) { if (a) *a=srcs[i]; if (b) *b=srcs[j]; return true; }
        }
    return false;
}
bool MainWindow::isDestOnline() const { return isWritableDir(QDir::cleanPath(m_destEdit->text())); }

void MainWindow::onValidate() {
    QStringList enabled;
    for (int i=0; i<m_sourceList->count(); ++i) {
        auto *it = m_sourceList->item(i);
        if (it->checkState() == Qt::Checked) enabled << QDir::cleanPath(it->text());
    }
    const QString dest = QDir::cleanPath(m_destEdit->text());

    QString err;
    if (enabled.isEmpty()) err = tr("请至少选择一个源目录。");
    else if (dest.isEmpty()) err = tr("请选择备份目标目录（移动硬盘上的文件夹）。");
    else if (!isWritableDir(dest)) err = tr("目标目录不可写或不存在（可能未插入移动硬盘）。");
    else {
        QString a,b;
        if (anySourceInsideAnother(&a,&b)) err = tr("源目录互为包含：\n%1\n包含了\n%2").arg(a, b);
        else for (const auto &s : enabled) {
                if (s.compare(dest, Qt::CaseInsensitive)==0) { err = tr("目标目录与源目录相同：%1").arg(dest); break; }
                if (isSubPath(s, dest)) { err = tr("目标目录在源目录内：\n源：%1\n目标：%2").arg(s, dest); break; }
            }
    }

    if (!err.isEmpty()) { m_statusLbl->setText(err); m_btnStart->setEnabled(false); }
    else { m_statusLbl->setText(tr("就绪。可手动开始，也可使用自动化策略。")); m_btnStart->setEnabled(true); }

    saveSettings();
    refreshWatcher();
}

// ========== 任务表 ==========
int MainWindow::addJobRow(const QString& src, const QString& dst) {
    const int row = m_jobs->rowCount();
    m_jobs->insertRow(row);
    m_jobs->setItem(row, 0, new QTableWidgetItem(src));
    m_jobs->setItem(row, 1, new QTableWidgetItem(dst));
    auto *bar = new QProgressBar(this);
    bar->setRange(0,100); bar->setValue(0); bar->setFormat(QStringLiteral("%p%"));
    m_jobs->setCellWidget(row, 2, bar);
    m_jobs->setItem(row, 3, new QTableWidgetItem("0 MB/s"));
    m_jobs->setItem(row, 4, new QTableWidgetItem("--:--:--"));
    m_jobs->setItem(row, 5, new QTableWidgetItem(tr("排队中")));
    addOpButtons(row);

    m_rowSpeedBps.resize(m_jobs->rowCount());
    m_rowRemainBytes.resize(m_jobs->rowCount());
    m_rowSpeedBps[row] = 0.0;
    m_rowRemainBytes[row] = 0;
    return row;
}
void MainWindow::addOpButtons(int row) {
    auto *w = new QWidget(this);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0,0,0,0);
    auto *btnPause  = new QToolButton(w); btnPause->setText(tr("暂停")); btnPause->setObjectName("pause");
    auto *btnResume = new QToolButton(w); btnResume->setText(tr("继续")); btnResume->setObjectName("resume");
    auto *btnCancel = new QToolButton(w); btnCancel->setText(tr("取消")); btnCancel->setObjectName("cancel");
    btnResume->setEnabled(false);
    btnPause->setProperty("row", row);
    btnResume->setProperty("row", row);
    btnCancel->setProperty("row", row);
    h->addWidget(btnPause); h->addWidget(btnResume); h->addWidget(btnCancel); h->addStretch(1);
    m_jobs->setCellWidget(row, 6, w);

    connect(btnPause,  &QToolButton::clicked, this, &MainWindow::onPauseRow);
    connect(btnResume, &QToolButton::clicked, this, &MainWindow::onResumeRow);
    connect(btnCancel, &QToolButton::clicked, this, &MainWindow::onCancelRow);

    // 追加：设置标准图标
    btnPause->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    btnResume->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    btnCancel->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
}
void MainWindow::updateGlobalStats() {
    double totalBps = 0.0; qint64 totalRemain = 0;
    for (int i=0;i<m_jobs->rowCount();++i) { totalBps += m_rowSpeedBps.value(i,0.0); totalRemain += m_rowRemainBytes.value(i,0); }
    const qint64 eta = (totalBps>1.0)? qint64(totalRemain/totalBps) : -1;
    m_totalSpeed->setText(tr("总速率：%1").arg(humanSpeed(totalBps)));
    m_totalEta->setText(tr("剩余时间：%1").arg(humanEta(eta)));
}

// ========== 启动任务（含空间预检） ==========
void MainWindow::onStartBackup() {
    if (!isDestOnline()) { statusBar()->showMessage(tr("目标目录不可用（设备离线）。"), 4000); return; }

    QStringList srcs;
    for (int i=0;i<m_sourceList->count();++i) {
        auto *it=m_sourceList->item(i);
        if (it->checkState()==Qt::Checked) srcs<<it->text();
    }
    const QString dst = m_destEdit->text();
    if (srcs.isEmpty() || dst.isEmpty()) return;

    // 粗略空间预检（未考虑去重/版本）
    qint64 totalNeed = 0;
    for (const QString& src : srcs) {
        QDirIterator it(src, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); totalNeed += it.fileInfo().size(); }
    }
    QStorageInfo st(dst);
    if (st.isValid()) {
        qint64 avail = st.bytesAvailable();
        if (avail < totalNeed * 1.1) {
            if (QMessageBox::warning(this, tr("空间不足"),
                                     tr("目标卷可用空间约 %1 MB，估计需求 %2 MB。\n空间可能不足，继续吗？")
                                         .arg(avail/1024/1024).arg(totalNeed/1024/1024),
                                     QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::No) {
                return;
            }
        }
    }

    m_backupRunning = true;
    m_failedBySrc.clear();
    m_failedList->clear();

    const qint64 speedLimitBps = qint64(m_spinSpeedLimitMB->value()) * 1024 * 1024;
    const int    retentionDays = m_spinRetentionDays->value();

    for (const auto& src : srcs) {
        const int row = addJobRow(src, dst);

        auto *worker = new BackupWorker({
            src, dst,
            /*verify*/true, /*retries*/3,
            /*ignore*/ splitPatterns(m_ignoreEdit->text()),
            /*whitelist*/ {},
            /*speedLimit*/ speedLimitBps,
            /*keepVersionsOnChange*/ true,
            /*keepDeletedInVault*/   true,
            /*retentionDays*/        retentionDays
        });

        auto *th = new QThread(this);
        th->setObjectName(QStringLiteral("BackupWorker:%1").arg(src));
        worker->moveToThread(th);

        Task tk; tk.src=src; tk.dst=dst; tk.worker=worker; tk.thread=th; tk.row=row; tk.paused=false;
        m_tasks.push_back(tk);

        // 启动
        connect(th, &QThread::started, worker, &BackupWorker::run);

        // 设备事件
        connect(worker, &BackupWorker::deviceOffline, this, &MainWindow::onWorkerDeviceOffline);
        connect(worker, &BackupWorker::deviceOnline,  this, &MainWindow::onWorkerDeviceOnline);

        // 进度/状态
        connect(worker, &BackupWorker::stateChanged, this, [=](const QString& s){ m_jobs->item(row,5)->setText(s); });
        connect(worker, &BackupWorker::progressUpdated, this, [=](qint64 done, qint64 total){
            auto *bar = qobject_cast<QProgressBar*>(m_jobs->cellWidget(row,2)); if(!bar) return;
            int pct = total>0 ? int((done*100)/qMax<qint64>(total,1)) : 0;
            bar->setValue(pct);
            bar->setFormat(QString("%1%  (%2 / %3 MB)").arg(pct).arg(done/1024/1024).arg(total/1024/1024));
            m_rowRemainBytes[row] = qMax<qint64>(total - done, 0);
            updateGlobalStats();
        });
        connect(worker, &BackupWorker::speedUpdated, this, [=](double bps){
            m_jobs->item(row,3)->setText(humanSpeed(bps));
            m_rowSpeedBps[row] = bps; updateGlobalStats();
        });
        connect(worker, &BackupWorker::etaUpdated, this, [=](qint64 sec){ m_jobs->item(row,4)->setText(humanEta(sec)); });
        connect(worker, &BackupWorker::fileFinished, this, [=](const QString& rel, bool ok, const QString& err){
            if (!ok) {
                m_failedBySrc[src] << rel;
                m_failedList->addItem(src + " :: " + rel + " :: " + err);
            }
        });
        connect(worker, &BackupWorker::versionCreated, this, [=](const QString&, const QString& path, const QString& meta){
            auto *it = new QListWidgetItem(QFileInfo(path).fileName());
            it->setToolTip(path);
            it->setData(Qt::UserRole, path);
            it->setData(Qt::UserRole+1, meta);
            m_versionsList->addItem(it);
        });
        connect(worker, &BackupWorker::deletedStashed, this, [=](const QString&, const QString& path, const QString& meta){
            auto *it = new QListWidgetItem(QFileInfo(path).fileName());
            it->setToolTip(path);
            it->setData(Qt::UserRole, path);
            it->setData(Qt::UserRole+1, meta);
            m_deletedList->addItem(it);
        });

        // !!! 正确的收口顺序：worker finished -> 线程 quit；对象 deleteLater 均在 finished 之后
        connect(worker, &BackupWorker::finished, th,     &QThread::quit);
        connect(worker, &BackupWorker::finished, worker, &QObject::deleteLater);
        connect(th,      &QThread::finished,     th,     &QObject::deleteLater);

        // UI 收尾（不负责删除对象）
        connect(worker, &BackupWorker::finished, this, [=](bool ok, const QString&){
            m_jobs->item(row,5)->setText(ok ? tr("完成") : tr("失败"));
            m_rowSpeedBps[row] = 0.0; updateGlobalStats();
            for (auto &t : m_tasks) if (t.row==row) { t.worker=nullptr; t.thread=nullptr; }

            bool anyRunning=false;
            for (int r=0;r<m_jobs->rowCount();++r) {
                const QString st=m_jobs->item(r,5)->text();
                if (st.contains(tr("复制")) || st.contains(tr("校验")) || st.contains(tr("排队"))) { anyRunning=true; break; }
            }
            if (!anyRunning) {
                m_backupRunning=false;
                if (m_chkAutoOnClose->isChecked() && m_pendingChanges) onStabilityTick();
            }
        });

        th->start();
    }
}

// ========== 行级操作 ==========
void MainWindow::onPauseRow() {
    auto *btn = qobject_cast<QToolButton*>(sender()); if(!btn) return;
    int row = btn->property("row").toInt();
    for (auto &t : m_tasks) if (t.row==row && t.worker) {
            t.worker->requestPause(true); t.paused=true;
            auto *w = m_jobs->cellWidget(row,6);
            QList<QToolButton*> buttons = w->findChildren<QToolButton*>();
            if (buttons.size()>=3) { buttons[0]->setEnabled(false); buttons[1]->setEnabled(true); }
            m_jobs->item(row,5)->setText(tr("已暂停"));
            break;
        }
}
void MainWindow::onResumeRow() {
    auto *btn = qobject_cast<QToolButton*>(sender()); if(!btn) return;
    int row = btn->property("row").toInt();
    for (auto &t : m_tasks) if (t.row==row && t.worker) {
            t.worker->requestPause(false); t.paused=false;
            auto *w = m_jobs->cellWidget(row,6);
            QList<QToolButton*> buttons = w->findChildren<QToolButton*>();
            if (buttons.size()>=3) { buttons[0]->setEnabled(true); buttons[1]->setEnabled(false); }
            m_jobs->item(row,5)->setText(tr("复制中"));
            break;
        }
}
void MainWindow::onCancelRow() {
    auto *btn = qobject_cast<QToolButton*>(sender()); if(!btn) return;
    int row = btn->property("row").toInt();
    for (auto &t : m_tasks) if (t.row==row && t.worker) {
            t.worker->requestStop();
            m_jobs->item(row,5)->setText(tr("取消中…"));
            auto *w = m_jobs->cellWidget(row,6);
            QList<QToolButton*> buttons = w->findChildren<QToolButton*>();
            for (auto *b : buttons) b->setEnabled(false);
            break;
        }
}

// ========== 失败重试 ==========
void MainWindow::onRetryFailed() {
    if (m_failedBySrc.isEmpty()) { QMessageBox::information(this, tr("提示"), tr("没有失败文件需要重试。")); return; }
    if (!isDestOnline()) { QMessageBox::warning(this, tr("设备离线"), tr("目标设备不在线，无法重试。")); return; }

    const qint64 speedLimitBps = qint64(m_spinSpeedLimitMB->value()) * 1024 * 1024;
    const int    retentionDays = m_spinRetentionDays->value();

    for (auto it = m_failedBySrc.begin(); it != m_failedBySrc.end(); ++it) {
        const QString src = it.key();
        const QStringList rels = it.value();
        const QString dst = m_destEdit->text();
        if (rels.isEmpty()) continue;

        const int row = addJobRow(src, dst);
        auto *worker = new BackupWorker({
            src, dst,
            /*verify*/true, /*retries*/3,
            /*ignore*/ splitPatterns(m_ignoreEdit->text()),
            /*whitelist*/ rels,
            /*speedLimit*/ speedLimitBps,
            /*keepVersionsOnChange*/ true,
            /*keepDeletedInVault*/   true,
            /*retentionDays*/        retentionDays
        });
        auto *th = new QThread(this);
        th->setObjectName(QStringLiteral("BackupWorker:Retry:%1").arg(src));
        worker->moveToThread(th);

        Task tk; tk.src=src; tk.dst=dst; tk.worker=worker; tk.thread=th; tk.row=row; tk.paused=false;
        m_tasks.push_back(tk);

        connect(th, &QThread::started, worker, &BackupWorker::run);
        connect(worker, &BackupWorker::deviceOffline, this, &MainWindow::onWorkerDeviceOffline);
        connect(worker, &BackupWorker::deviceOnline,  this, &MainWindow::onWorkerDeviceOnline);

        connect(worker, &BackupWorker::stateChanged, this, [=](const QString& s){ m_jobs->item(row,5)->setText(s); });
        connect(worker, &BackupWorker::progressUpdated, this, [=](qint64 done, qint64 total){
            auto *bar = qobject_cast<QProgressBar*>(m_jobs->cellWidget(row,2)); if(!bar) return;
            int pct = total>0 ? int((done*100)/qMax<qint64>(total,1)) : 0;
            bar->setValue(pct);
            bar->setFormat(QString("%1%  (%2 / %3 MB)").arg(pct).arg(done/1024/1024).arg(total/1024/1024));
            m_rowRemainBytes[row] = qMax<qint64>(total - done, 0);
            updateGlobalStats();
        });
        connect(worker, &BackupWorker::speedUpdated, this, [=](double bps){
            m_jobs->item(row,3)->setText(humanSpeed(bps)); m_rowSpeedBps[row]=bps; updateGlobalStats();
        });
        connect(worker, &BackupWorker::etaUpdated, this, [=](qint64 sec){ m_jobs->item(row,4)->setText(humanEta(sec)); });
        connect(worker, &BackupWorker::fileFinished, this, [=](const QString& rel, bool ok, const QString& err){
            if (!ok) m_failedList->addItem(src + " :: " + rel + " :: " + err);
        });
        connect(worker, &BackupWorker::versionCreated, this, [=](const QString&, const QString& path, const QString& meta){
            auto *itItem = new QListWidgetItem(QFileInfo(path).fileName());
            itItem->setToolTip(path);
            itItem->setData(Qt::UserRole, path);
            itItem->setData(Qt::UserRole+1, meta);
            m_versionsList->addItem(itItem);
        });
        connect(worker, &BackupWorker::deletedStashed, this, [=](const QString&, const QString& path, const QString& meta){
            auto *itItem = new QListWidgetItem(QFileInfo(path).fileName());
            itItem->setToolTip(path);
            itItem->setData(Qt::UserRole, path);
            itItem->setData(Qt::UserRole+1, meta);
            m_deletedList->addItem(itItem);
        });

        // 正确收口
        connect(worker, &BackupWorker::finished, th,     &QThread::quit);
        connect(worker, &BackupWorker::finished, worker, &QObject::deleteLater);
        connect(th,      &QThread::finished,     th,     &QObject::deleteLater);

        connect(worker, &BackupWorker::finished, this, [=](bool ok, const QString&){
            m_jobs->item(row,5)->setText(ok ? tr("完成") : tr("失败"));
            m_rowSpeedBps[row]=0.0; updateGlobalStats();
            for (auto &t : m_tasks) if (t.row==row) { t.worker=nullptr; t.thread=nullptr; }
        });

        th->start();
    }

    m_failedBySrc.clear();
    m_failedList->clear();
}

// ========== 自动化 ==========
void MainWindow::onAutoOptionsChanged() {
    m_timerDevice->start(m_spinDevChkMin->value()*60*1000);
    if (m_chkAutoInterval->isChecked()) m_timerInterval->start(m_spinIntervalMin->value()*60*1000);
    else m_timerInterval->stop();

    // 智能模式轮询
    if (m_chkSmart->isChecked()) m_timerSmart->start(m_spinSmartPollSec->value()*1000);
    else                         m_timerSmart->stop();

    refreshWatcher();
}
void MainWindow::onDeviceCheckTick() {
    const bool online = isDestOnline();
    if (online != m_deviceOnline) {
        m_deviceOnline = online;
        if (online) {
            statusBar()->showMessage(tr("设备已在线，可自动备份。"), 3000);
            // 正在跑且被 UI 暂停过 → 恢复
            if (m_backupRunning) resumeAllTasks(false);
            if (m_chkAutoOnClose->isChecked() && !m_backupRunning && m_pendingChanges)
                tryStartAutoBackup(tr("设备恢复在线"));
        } else {
            statusBar()->showMessage(tr("设备离线：暂停自动备份。"), 4000);
            if (m_backupRunning) pauseAllTasks(false);
        }
    }
}
void MainWindow::onAutoIntervalTick() { if (m_chkAutoInterval->isChecked()) tryStartAutoBackup(tr("定时触发")); }
void MainWindow::onStabilityTick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 windowMs = qint64(m_spinStabSec->value())*1000;
    if (now - m_lastChangeMs < windowMs) { m_timerStab->start(int(windowMs - (now - m_lastChangeMs))); return; }
    tryStartAutoBackup(tr("稳定窗口结束"));
}
void MainWindow::onWatchedPathChanged(const QString& path) {
    if (!m_chkAutoOnClose->isChecked()) return;

    // 新增：把新出现的子目录递归加入监听
    QDir base(path);
    if (base.exists()) {
        QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QString sub = QDir(it.next()).absolutePath();
            if (!m_watchedDirs.contains(sub)) {
                m_watcher->addPath(sub);
                m_watchedDirs.insert(sub);
            }
        }
    }

    m_pendingChanges = true;
    m_lastChangeMs = QDateTime::currentMSecsSinceEpoch();
    m_timerStab->start(m_spinStabSec->value()*1000);
}
void MainWindow::tryStartAutoBackup(const QString& reason) {
    if (m_backupRunning) return;
    if (!isDestOnline()) return;
    onValidate();
    if (!m_btnStart->isEnabled()) return;
    statusBar()->showMessage(tr("自动备份触发：%1").arg(reason), 3000);
    m_pendingChanges = false;
    onStartBackup();
}

// ========== 递归监听 ==========
void MainWindow::rebuildRecursiveWatch(const QString& root) {
    if (root.isEmpty()) return;
    QDir base(root);
    if (!base.exists()) return;

    const QString rootAbs = QDir(root).absolutePath();
    if (!m_watchedDirs.contains(rootAbs)) {
        m_watcher->addPath(rootAbs);
        m_watchedDirs.insert(rootAbs);
    }
    QDirIterator it(root, QDir::Dirs|QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString d = QDir(it.next()).absolutePath();
        if (!m_watchedDirs.contains(d)) {
            m_watcher->addPath(d);
            m_watchedDirs.insert(d);
        }
    }
}
void MainWindow::refreshWatcher() {
    for (const auto &d : m_watcher->directories()) m_watcher->removePath(d);
    for (const auto &f : m_watcher->files())       m_watcher->removePath(f);
    m_watchedDirs.clear();

    for (int i=0;i<m_sourceList->count();++i) {
        auto *it=m_sourceList->item(i);
        if (it->checkState()==Qt::Checked) rebuildRecursiveWatch(QDir::cleanPath(it->text()));
    }
}

// ========== 忽略规则 ==========
QStringList MainWindow::splitPatterns(const QString& text) const {
    QStringList raw = text.split(QRegularExpression(R"([;\n])"), Qt::SkipEmptyParts);
    for (QString& s : raw) s = s.trimmed();
    raw.erase(std::remove_if(raw.begin(), raw.end(),
                             [](const QString& s){ return s.isEmpty(); }),
              raw.end());
    return raw;
}

// ========== 设置持久化 ==========
void MainWindow::loadSettings() {
    QSettings s;
    int n = s.beginReadArray("sources");
    for (int i=0;i<n;++i) {
        s.setArrayIndex(i);
        const QString path = s.value("path").toString();
        const bool enabled = s.value("enabled", true).toBool();
        if (!path.isEmpty()) {
            auto *item = new QListWidgetItem(QDir::cleanPath(path));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
            m_sourceList->addItem(item);
        }
    }
    s.endArray();
    m_destEdit->setText(s.value("dest").toString());
    m_ignoreEdit->setText(s.value("ignore/patterns", "").toString());

    m_chkAutoInterval->setChecked(s.value("auto/interval/enabled", false).toBool());
    m_spinIntervalMin->setValue(s.value("auto/interval/minutes", 30).toInt());
    m_chkAutoOnClose->setChecked(s.value("auto/onclose/enabled", false).toBool());
    m_spinStabSec->setValue(s.value("auto/onclose/stab_sec", 120).toInt());
    m_spinDevChkMin->setValue(s.value("auto/devcheck/minutes", 60).toInt());

    // 高级
    m_spinRetentionDays->setValue(s.value("adv/retention_days", 7).toInt());
    m_spinSpeedLimitMB->setValue(s.value("adv/speed_limit_mb", 0).toInt());
    m_chkSmart->setChecked(s.value("adv/smart/enabled", false).toBool());
    m_spinSmartCpuHi->setValue(s.value("adv/smart/cpu_hi", 65).toInt());
    m_spinSmartPollSec->setValue(s.value("adv/smart/poll_sec", 5).toInt());
}
void MainWindow::saveSettings() const {
    QSettings s;
    s.beginWriteArray("sources");
    int idx = 0;
    for (int i=0;i<m_sourceList->count();++i) {
        auto *it = m_sourceList->item(i);
        s.setArrayIndex(idx++);
        s.setValue("path", it->text());
        s.setValue("enabled", it->checkState() == Qt::Checked);
    }
    s.endArray();
    s.setValue("dest", m_destEdit->text());
    s.setValue("ignore/patterns", m_ignoreEdit->text());

    s.setValue("auto/interval/enabled", m_chkAutoInterval->isChecked());
    s.setValue("auto/interval/minutes", m_spinIntervalMin->value());
    s.setValue("auto/onclose/enabled",  m_chkAutoOnClose->isChecked());
    s.setValue("auto/onclose/stab_sec", m_spinStabSec->value());
    s.setValue("auto/devcheck/minutes", m_spinDevChkMin->value());

    // 高级
    s.setValue("adv/retention_days", m_spinRetentionDays->value());
    s.setValue("adv/speed_limit_mb", m_spinSpeedLimitMB->value());
    s.setValue("adv/smart/enabled",  m_chkSmart->isChecked());
    s.setValue("adv/smart/cpu_hi",   m_spinSmartCpuHi->value());
    s.setValue("adv/smart/poll_sec", m_spinSmartPollSec->value());
}

// ========== 线程收尾 ==========
void MainWindow::stopAllTasks(int waitMs) {
    // 1) 请求停止并唤醒
    for (auto &t : m_tasks) {
        if (t.worker) {
            t.worker->requestPause(false);
            t.worker->requestStop();
            QMetaObject::invokeMethod(t.worker, [](){}, Qt::QueuedConnection);
        }
    }
    // 2) 请求线程退出事件循环
    for (auto &t : m_tasks) {
        if (t.thread && t.thread->isRunning()) t.thread->quit();
    }
    // 3) 等待（带事件处理），让 finished/quit 等信号有机会送达
    QElapsedTimer timer; timer.start();
    const int slice = 50;
    while (true) {
        bool anyRunning = false;
        for (auto &t : m_tasks) {
            if (t.thread && t.thread->isRunning()) { anyRunning = true; break; }
        }
        if (!anyRunning) break;
        if (waitMs > 0 && timer.elapsed() >= waitMs) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, slice);
        QThread::msleep(slice);
    }
    // 4) 兜底等待
    for (auto &t : m_tasks) {
        if (t.thread && t.thread->isRunning()) {
            t.thread->requestInterruption();
            t.thread->wait(2000);
        }
    }
}


// ========== 元数据面板（扫描/恢复） ==========
QString MainWindow::metaRootOfDest() const {
    const QString dest = QDir::cleanPath(m_destEdit->text());
    if (dest.isEmpty()) return {};
    return QDir(dest).absoluteFilePath(".plugbackup_meta");
}
void MainWindow::populateVaultLists() {
    m_versionsList->clear();
    m_deletedList->clear();
    const QString root = metaRootOfDest();
    if (root.isEmpty() || !QDir(root).exists()) return;

    auto scanOne = [&](const QString& subdir, QListWidget* list){
        const QString base = QDir(root).absoluteFilePath(subdir);
        QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QString file = it.filePath();
            if (file.endsWith(".json", Qt::CaseInsensitive)) continue;
            auto *item = new QListWidgetItem(QFileInfo(file).fileName());
            item->setToolTip(file);
            item->setData(Qt::UserRole, file);
            const QString meta = file + ".json";
            if (QFileInfo::exists(meta)) item->setData(Qt::UserRole+1, meta);
            list->addItem(item);
        }
    };

    scanOne("versions", m_versionsList);
    scanOne("deleted",  m_deletedList);
}
void MainWindow::onScanVault() {
    populateVaultLists();
    statusBar()->showMessage(tr("已扫描版本/删除留存"), 2000);
}
static bool readOrigAbsFromMeta(const QString& metaPath, QString* outOrigAbs, QString* outRel=nullptr, QString* outSrcRoot=nullptr) {
    QFile f(metaPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return false;
    const auto o = doc.object();
    const QString orig = o.value("origAbs").toString();
    if (orig.isEmpty()) return false;
    if (outOrigAbs) *outOrigAbs = orig;
    if (outRel)     *outRel     = o.value("rel").toString();
    if (outSrcRoot) *outSrcRoot = o.value("srcRoot").toString();
    return true;
}
static bool copyFileWithDirs(const QString& from, const QString& to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (QFile::exists(to)) QFile::remove(to);
    return QFile::copy(from, to);
}
void MainWindow::onRestoreSelectedVersion() {
    auto *it = m_versionsList->currentItem();
    if (!it) { QMessageBox::information(this, tr("提示"), tr("请先在“历史版本”中选择一项。")); return; }
    const QString payload = itemPayloadPath(it);
    const QString meta    = itemMetaPath(it);
    if (payload.isEmpty() || meta.isEmpty()) { QMessageBox::warning(this, tr("缺少元数据"), tr("无法定位原始路径。")); return; }

    QString origAbs, rel, srcRoot;
    if (!readOrigAbsFromMeta(meta, &origAbs, &rel, &srcRoot)) {
        QMessageBox::warning(this, tr("读取失败"), tr("无法解析元数据：%1").arg(meta));
        return;
    }
    if (QMessageBox::question(this, tr("恢复历史版本"),
                              tr("将把历史版本恢复到源文件位置：\n%1\n\n继续？").arg(origAbs)) != QMessageBox::Yes) return;

    if (!copyFileWithDirs(payload, origAbs)) {
        QMessageBox::warning(this, tr("恢复失败"), tr("拷贝失败：%1 → %2").arg(payload, origAbs));
        return;
    }
    statusBar()->showMessage(tr("已恢复历史版本 → %1").arg(origAbs), 3000);
}
void MainWindow::onRestoreSelectedDeleted() {
    auto *it = m_deletedList->currentItem();
    if (!it) { QMessageBox::information(this, tr("提示"), tr("请先在“删除留存”中选择一项。")); return; }
    const QString payload = itemPayloadPath(it);
    const QString meta    = itemMetaPath(it);
    if (payload.isEmpty() || meta.isEmpty()) { QMessageBox::warning(this, tr("缺少元数据"), tr("无法定位原始路径。")); return; }

    QString origAbs, rel, srcRoot;
    if (!readOrigAbsFromMeta(meta, &origAbs, &rel, &srcRoot)) {
        QMessageBox::warning(this, tr("读取失败"), tr("无法解析元数据：%1").arg(meta));
        return;
    }
    if (QMessageBox::question(this, tr("恢复删除留存"),
                              tr("将把删除留存恢复到源文件位置：\n%1\n\n继续？").arg(origAbs)) != QMessageBox::Yes) return;

    if (!copyFileWithDirs(payload, origAbs)) {
        QMessageBox::warning(this, tr("恢复失败"), tr("拷贝失败：%1 → %2").arg(payload, origAbs));
        return;
    }
    const QString dstPath = QDir(QDir::cleanPath(m_destEdit->text())).absoluteFilePath(rel);
    copyFileWithDirs(payload, dstPath);

    statusBar()->showMessage(tr("已恢复删除留存 → %1").arg(origAbs), 3000);
}

QString MainWindow::itemPayloadPath(QListWidgetItem* it) { return it ? it->data(Qt::UserRole).toString() : QString(); }
QString MainWindow::itemMetaPath(QListWidgetItem* it)    { return it ? it->data(Qt::UserRole+1).toString() : QString(); }

// ========== 智能模式 ==========
void MainWindow::pauseAllTasks(bool fromSmart) {
    for (auto &t : m_tasks) if (t.worker) {
            t.worker->requestPause(true); t.paused = true;
            if (t.row >= 0) {
                auto *w = m_jobs->cellWidget(t.row,6);
                if (w) {
                    QList<QToolButton*> buttons = w->findChildren<QToolButton*>();
                    if (buttons.size()>=3) { buttons[0]->setEnabled(false); buttons[1]->setEnabled(true); }
                }
                m_jobs->item(t.row,5)->setText(fromSmart ? tr("智能暂停") : tr("已暂停"));
            }
        }
    if (fromSmart) m_smartPaused = true;
}
void MainWindow::resumeAllTasks(bool fromSmart) {
    for (auto &t : m_tasks) if (t.worker) {
            t.worker->requestPause(false); t.paused = false;
            if (t.row >= 0) {
                auto *w = m_jobs->cellWidget(t.row,6);
                if (w) {
                    QList<QToolButton*> buttons = w->findChildren<QToolButton*>();
                    if (buttons.size()>=3) { buttons[0]->setEnabled(true); buttons[1]->setEnabled(false); }
                }
                m_jobs->item(t.row,5)->setText(tr("复制中"));
            }
        }
    if (fromSmart) m_smartPaused = false;
}

double MainWindow::sampleSystemCpuUsagePercent() {
#ifdef Q_OS_WIN
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;
    auto to64 = [](const FILETIME& ft)->quint64 {
        ULARGE_INTEGER ul; ul.LowPart = ft.dwLowDateTime; ul.HighPart = ft.dwHighDateTime; return ul.QuadPart;
    };
    quint64 idleNow   = to64(idle);
    quint64 kernelNow = to64(kernel);
    quint64 userNow   = to64(user);

    if (m_prevIdle==0 && m_prevKernel==0 && m_prevUser==0) {
        m_prevIdle = idleNow; m_prevKernel = kernelNow; m_prevUser = userNow;
        return 0.0;
    }
    quint64 idleDiff   = idleNow   - m_prevIdle;
    quint64 kernelDiff = kernelNow - m_prevKernel;
    quint64 userDiff   = userNow   - m_prevUser;

    m_prevIdle = idleNow; m_prevKernel = kernelNow; m_prevUser = userNow;

    quint64 total = kernelDiff + userDiff;
    if (total == 0) return 0.0;
    double busy = double(total - idleDiff) / double(total);
    if (busy < 0) busy = 0; if (busy > 1) busy = 1;
    return busy * 100.0;
#else
    // 其它平台可扩展 /proc/stat 采样；这里先返回低负载
    return 0.0;
#endif
}

void MainWindow::onSmartTick() {
    if (!m_chkSmart->isChecked()) return;
    if (!m_backupRunning) return;

    const double cpu = sampleSystemCpuUsagePercent();
    const int hi = m_spinSmartCpuHi->value();
    const int lo = std::max(hi - 10, 10); // 10% 回落滞后，避免抖动

    if (cpu >= hi) { m_smartBusyCount++; m_smartIdleCount = 0; }
    else if (cpu <= lo) { m_smartIdleCount++; m_smartBusyCount = 0; }
    else { /* 中间带，不计数 */ }

    // 连续两次繁忙才暂停；连续两次空闲才恢复
    if (!m_smartPaused && m_smartBusyCount >= 2) {
        statusBar()->showMessage(tr("智能模式：CPU= %1% ，自动暂停").arg(int(cpu)), 3000);
        pauseAllTasks(true);
    } else if (m_smartPaused && m_smartIdleCount >= 2) {
        statusBar()->showMessage(tr("智能模式：CPU= %1% ，自动恢复").arg(int(cpu)), 3000);
        resumeAllTasks(true);
    }
}


void MainWindow::onWorkerDeviceOffline(const QString& phase) {
    // 记录来源，避免多个任务重复弹窗
    m_offlineWorkers.insert(sender());

    // 仅在首次进入离线状态时弹一次非模态提示
    if (!m_offlineBox) {
        m_offlineBox = new QMessageBox(QMessageBox::Warning,
                                       tr("设备已断开"),
                                       tr("检测到备份目标设备被拔出或离线。\n"
                                          "当前备份已暂停，等待设备重新连接后将自动继续。\n\n"
                                          "阶段：%1").arg(phase.isEmpty()?tr("进行中"):phase),
                                       QMessageBox::Ok,
                                       this);
        m_offlineBox->setWindowModality(Qt::NonModal);
        m_offlineBox->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_offlineBox, &QObject::destroyed, this, [this]{ m_offlineBox = nullptr; });
        m_offlineBox->show();
        QApplication::beep();
    }

    // 让任务表状态更贴近事实（如果你愿意）：
    // pauseAllTasks(false); // 可选：UI 按钮状态立即切为“已暂停”
    statusBar()->showMessage(tr("目标设备离线，等待重新连接…"), 4000);
}

void MainWindow::onWorkerDeviceOnline() {
    // 某个 worker 恢复在线
    m_offlineWorkers.remove(sender());

    // 全部都恢复后，再关闭提示
    if (m_offlineWorkers.isEmpty() && m_offlineBox) {
        m_offlineBox->close();  // 触发 destroyed → 置空指针
    }
    statusBar()->showMessage(tr("目标设备已恢复在线，继续备份。"), 3000);
}

void MainWindow::applyTheme() {
    // 轻量、现代的浅色主题（Tailwind 风格色）
    const char* qss = R"(
    /* 背景与整体色 */
    QMainWindow { background:#f7f8fa; }
    QLabel { color:#111827; } /* 近黑 */

    /* 卡片式 GroupBox */
    // QGroupBox {
    //     background:#ffffff;
    //     border:1px solid #e5e7eb;      /* neutral-200 */
    //     border-radius:10px;
    //     margin-top:16px;               /* 为标题留位置 */
    // }
    // QGroupBox::title {
    //     subcontrol-origin: margin;
    //     left:12px; top:-8px;
    //     padding:0 6px;
    //     background:#ffffff;
    //     color:#111827;
    //     font-weight:600;
    // }
    QGroupBox {
        background:#ffffff;
        border:1px solid #e5e7eb;
        border-radius:10px;
        margin-top:28px;                 /* 为标题留足高度 */
    }
    QGroupBox::title {
        subcontrol-origin: margin;
        subcontrol-position: top left;
        left:12px;
        padding:0 6px;
        background:#ffffff;
        color:#111827;
        font-weight:600;
    }

    /* 文本输入与数值输入 */
    QLineEdit, QSpinBox, QComboBox, QTextEdit {
        background:#ffffff;
        border:1px solid #d1d5db;      /* neutral-300 */
        border-radius:8px;
        padding:6px 8px;
        selection-background-color:#3b82f6; /* primary */
    }
    QSpinBox::up-button, QSpinBox::down-button {
        width:18px; border:none; background:transparent;
        margin:1px;
    }

    /* 按钮（主/次/操作色） */
    QPushButton, QToolButton {
        background:#3b82f6;            /* primary */
        color:white; border:none; border-radius:8px;
        padding:6px 12px;
    }
    QPushButton:hover, QToolButton:hover { background:#2563eb; }
    QPushButton:pressed, QToolButton:pressed { background:#1d4ed8; }
    QPushButton:disabled, QToolButton:disabled { background:#9ca3af; color:#f3f4f6; }

    /* 行内操作按钮三色（通过 objectName 区分） */
    QToolButton#pause  { background:#f59e0b; }   /* amber-500 */
    QToolButton#pause:hover  { background:#d97706; }
    QToolButton#resume { background:#10b981; }   /* emerald-500 */
    QToolButton#resume:hover { background:#059669; }
    QToolButton#cancel { background:#ef4444; }   /* red-500 */
    QToolButton#cancel:hover { background:#dc2626; }

    /* 表格与表头 */
    QTableWidget {
        background:#ffffff;
        border:1px solid #e5e7eb;
        border-radius:10px;
        gridline-color:#eef2f7;
        alternate-background-color:#fafafa;
    }
    QHeaderView::section {
        background:#f3f4f6;
        color:#111827;
        border:none;
        padding:8px 10px;
        font-weight:600;
    }
    QTableCornerButton::section { background:#f3f4f6; border:none; }

    /* 进度条 */
    QProgressBar {
        border:1px solid #d1d5db; border-radius:8px;
        text-align:center; height:18px; background:#f3f4f6;
    }
    QProgressBar::chunk {
        border-radius:7px; background:#3b82f6;
    }

    /* 列表/多行文本 */
    QListWidget, QTextEdit {
        background:#ffffff;
        border:1px solid #e5e7eb;
        border-radius:10px;
    }

    /* 状态栏 */
    QStatusBar {
        background:#ffffff; border-top:1px solid #e5e7eb;
    }

    /* 更顺滑的滚动条（可选） */
    QScrollBar:vertical {
        background:transparent; width:10px; margin:4px;
    }
    QScrollBar::handle:vertical {
        background:#d1d5db; border-radius:5px; min-height:40px;
    }
    QScrollBar::handle:vertical:hover { background:#9ca3af; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }

    QScrollBar:horizontal {
        background:transparent; height:10px; margin:4px;
    }
    QScrollBar::handle:horizontal {
        background:#d1d5db; border-radius:5px; min-width:40px;
    }
    QScrollBar::handle:horizontal:hover { background:#9ca3af; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }
    )";

    qApp->setStyleSheet(qss);
}

void MainWindow::polishUi() {
    // 统一所有布局的间距
    for (auto *lay : findChildren<QLayout*>()) {
        lay->setSpacing(8);
        auto *gl = qobject_cast<QGridLayout*>(lay);
        if (gl) gl->setHorizontalSpacing(10);
    }

    // 表格观感
    m_jobs->setAlternatingRowColors(true);
    m_jobs->setShowGrid(false);
    m_jobs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_jobs->verticalHeader()->setDefaultSectionSize(28);
    m_jobs->horizontalHeader()->setHighlightSections(false);
    m_jobs->horizontalHeader()->setStretchLastSection(true);
    m_jobs->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_jobs->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_jobs->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_jobs->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_jobs->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_jobs->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_jobs->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);

    // 操作列按钮补上标准图标
    for (int r = 0; r < m_jobs->rowCount(); ++r) {
        if (auto *w = m_jobs->cellWidget(r, 6)) {
            const auto btns = w->findChildren<QToolButton*>();
            for (auto *b : btns) {
                if (b->objectName() == "pause")
                    b->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
                else if (b->objectName() == "resume")
                    b->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
                else if (b->objectName() == "cancel")
                    b->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
            }
        }
    }

    // 给所有 GroupBox 加一个柔和阴影（卡片感）
    for (QGroupBox *g : findChildren<QGroupBox*>()) {
        g->setAttribute(Qt::WA_StyledBackground); // 让 QSS 背景生效
        if (auto *lay = g->layout()) lay->setContentsMargins(12, 8, 12, 12);

        auto *fx = new QGraphicsDropShadowEffect(g);
        fx->setBlurRadius(16);
        fx->setOffset(0, 2);
        fx->setColor(QColor(0, 0, 0, 40));
        g->setGraphicsEffect(fx);
    }
}
