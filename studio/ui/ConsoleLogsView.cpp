#include "ui/ConsoleLogsView.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>

ConsoleLogsView::ConsoleLogsView(QWidget* parent) : QWidget(parent) {
    setupUi();

    if (LogManager::instance()) {
        connect(LogManager::instance(), &LogManager::logAppended, this, &ConsoleLogsView::onLogAppended);
        connect(LogManager::instance(), &LogManager::logsCleared, this, &ConsoleLogsView::refreshLogs);
    }
    refreshLogs();
}

void ConsoleLogsView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // 1. Top Toolbar
    auto toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(8);

    auto title = new QLabel("Console Logs", this);
    QFont titleFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    title->setFont(titleFont);
    toolbarLayout->addWidget(title);

    m_logCountLabel = new QLabel(this);
    QPalette countPal = m_logCountLabel->palette();
    countPal.setColor(QPalette::WindowText, countPal.color(QPalette::PlaceholderText));
    m_logCountLabel->setPalette(countPal);
    toolbarLayout->addWidget(m_logCountLabel);

    toolbarLayout->addSpacing(8);

    // Search bar
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search logs...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumWidth(180);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ConsoleLogsView::refreshLogs);
    toolbarLayout->addWidget(m_searchEdit);

    // Filter Combo (Log Level)
    auto levelLabel = new QLabel("Level:", this);
    toolbarLayout->addWidget(levelLabel);

    m_levelFilterCombo = new QComboBox(this);
    m_levelFilterCombo->addItem("Off", static_cast<int>(LogLevel::Off));
    m_levelFilterCombo->addItem("Error", static_cast<int>(LogLevel::Error));
    m_levelFilterCombo->addItem("Warn", static_cast<int>(LogLevel::Warn));
    m_levelFilterCombo->addItem("Info", static_cast<int>(LogLevel::Info));
    m_levelFilterCombo->addItem("Debug", static_cast<int>(LogLevel::Debug));
    m_levelFilterCombo->addItem("Trace", static_cast<int>(LogLevel::Trace));
    m_levelFilterCombo->setMinimumWidth(100);

    if (LogManager::instance()) {
        int initialIdx = m_levelFilterCombo->findData(static_cast<int>(LogManager::instance()->logLevel()));
        if (initialIdx >= 0) {
            m_levelFilterCombo->setCurrentIndex(initialIdx);
        }
    }

    connect(m_levelFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        LogLevel level = static_cast<LogLevel>(m_levelFilterCombo->itemData(idx).toInt());
        if (LogManager::instance()) {
            LogManager::instance()->setLogLevel(level);
        }
        refreshLogs();
    });
    toolbarLayout->addWidget(m_levelFilterCombo);

    toolbarLayout->addStretch();

    // Auto-scroll checkbox
    m_autoScrollCheck = new QCheckBox("Auto-scroll", this);
    m_autoScrollCheck->setChecked(true);
    toolbarLayout->addWidget(m_autoScrollCheck);

    // Copy button
    m_copyBtn = new QPushButton("Copy", this);
    connect(m_copyBtn, &QPushButton::clicked, this, &ConsoleLogsView::copyAllLogs);
    toolbarLayout->addWidget(m_copyBtn);

    // Clear button
    m_clearBtn = new QPushButton("Clear", this);
    connect(m_clearBtn, &QPushButton::clicked, [this]() {
        if (LogManager::instance()) {
            LogManager::instance()->clear();
        }
    });
    toolbarLayout->addWidget(m_clearBtn);

    mainLayout->addLayout(toolbarLayout);

    // 2. Log Table View
    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({"Timestamp", "Level", "Message"});
    m_table->horizontalHeader()->setVisible(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->setShowGrid(false);
    m_table->setWordWrap(true);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        Q_UNUSED(pos);
        QMenu menu(this);
        auto copySel = menu.addAction("Copy Selected");
        connect(copySel, &QAction::triggered, this, [this]() {
            auto ranges = m_table->selectedRanges();
            if (ranges.isEmpty())
                return;
            QStringList lines;
            for (const auto& range : ranges) {
                for (int r = range.topRow(); r <= range.bottomRow(); ++r) {
                    QString t = m_table->item(r, 0) ? m_table->item(r, 0)->text() : "";
                    QString l = m_table->item(r, 1) ? m_table->item(r, 1)->text() : "";
                    QString m = m_table->item(r, 2) ? m_table->item(r, 2)->text() : "";
                    lines.append(QString("[%1] [%2] %3").arg(t, l, m));
                }
            }
            QGuiApplication::clipboard()->setText(lines.join("\n"));
        });

        auto copyAll = menu.addAction("Copy All Logs");
        connect(copyAll, &QAction::triggered, this, &ConsoleLogsView::copyAllLogs);

        menu.addSeparator();
        auto clearAct = menu.addAction("Clear Logs");
        connect(clearAct, &QAction::triggered, this, [this]() {
            if (LogManager::instance()) {
                LogManager::instance()->clear();
            }
        });

        menu.exec(QCursor::pos());
    });

    mainLayout->addWidget(m_table);
}

void ConsoleLogsView::refreshLogs() {
    m_table->setRowCount(0);
    if (!LogManager::instance())
        return;

    auto captionMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    captionMono.setPointSize(11);

    auto bodyMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    bodyMono.setPointSize(13);

    QString filterText = m_searchEdit ? m_searchEdit->text().trimmed() : QString();

    auto entries = LogManager::instance()->logs();
    int matchCount = 0;
    for (const auto& entry : entries) {
        if (!filterText.isEmpty() && !entry.message.contains(filterText, Qt::CaseInsensitive)) {
            continue;
        }

        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss.zzz"));
        timeItem->setFont(captionMono);
        timeItem->setForeground(palette().color(QPalette::PlaceholderText));
        timeItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

        auto levelItem = new QTableWidgetItem(logLevelToString(entry.level));
        levelItem->setFont(captionMono);
        levelItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);
        if (entry.level == LogLevel::Error) {
            levelItem->setForeground(QColor(230, 50, 50));
        } else if (entry.level == LogLevel::Warn) {
            levelItem->setForeground(QColor(230, 140, 20));
        } else if (entry.level == LogLevel::Info) {
            levelItem->setForeground(QColor(40, 160, 80));
        } else if (entry.level == LogLevel::Debug) {
            levelItem->setForeground(QColor(80, 140, 220));
        } else {
            levelItem->setForeground(palette().color(QPalette::PlaceholderText));
        }

        auto msgItem = new QTableWidgetItem(entry.message);
        msgItem->setFont(bodyMono);
        msgItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

        m_table->setItem(row, 0, timeItem);
        m_table->setItem(row, 1, levelItem);
        m_table->setItem(row, 2, msgItem);
        ++matchCount;
    }

    if (m_logCountLabel) {
        m_logCountLabel->setText(QString("%1 logs").arg(matchCount));
    }

    if (m_autoScrollCheck && m_autoScrollCheck->isChecked()) {
        m_table->scrollToBottom();
    }
}

void ConsoleLogsView::onLogAppended(const LogEntry& entry) {
    QString filterText = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (!filterText.isEmpty() && !entry.message.contains(filterText, Qt::CaseInsensitive)) {
        return;
    }

    auto captionMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    captionMono.setPointSize(11);

    auto bodyMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    bodyMono.setPointSize(13);

    int row = m_table->rowCount();
    m_table->insertRow(row);

    auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss.zzz"));
    timeItem->setFont(captionMono);
    timeItem->setForeground(palette().color(QPalette::PlaceholderText));
    timeItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    auto levelItem = new QTableWidgetItem(logLevelToString(entry.level));
    levelItem->setFont(captionMono);
    levelItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);
    if (entry.level == LogLevel::Error) {
        levelItem->setForeground(QColor(230, 50, 50));
    } else if (entry.level == LogLevel::Warn) {
        levelItem->setForeground(QColor(230, 140, 20));
    } else if (entry.level == LogLevel::Info) {
        levelItem->setForeground(QColor(40, 160, 80));
    } else if (entry.level == LogLevel::Debug) {
        levelItem->setForeground(QColor(80, 140, 220));
    } else {
        levelItem->setForeground(palette().color(QPalette::PlaceholderText));
    }

    auto msgItem = new QTableWidgetItem(entry.message);
    msgItem->setFont(bodyMono);
    msgItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_table->setItem(row, 0, timeItem);
    m_table->setItem(row, 1, levelItem);
    m_table->setItem(row, 2, msgItem);

    constexpr int kMaxTableRows = 2000;
    while (m_table->rowCount() > kMaxTableRows) {
        m_table->removeRow(0);
    }

    if (m_logCountLabel) {
        m_logCountLabel->setText(QString("%1 logs").arg(m_table->rowCount()));
    }

    if (m_autoScrollCheck && m_autoScrollCheck->isChecked()) {
        m_table->scrollToBottom();
    }
}

void ConsoleLogsView::copyAllLogs() {
    if (!LogManager::instance())
        return;
    auto entries = LogManager::instance()->logs();
    QStringList textRows;
    for (const auto& entry : entries) {
        textRows.append(
            QString("[%1] [%2] %3")
                .arg(entry.timestamp.toString(Qt::ISODateWithMs), logLevelToString(entry.level), entry.message));
    }
    QGuiApplication::clipboard()->setText(textRows.join("\n"));
}
