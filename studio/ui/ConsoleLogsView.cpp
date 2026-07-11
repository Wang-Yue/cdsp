#include "ui/ConsoleLogsView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QAction>
#include <QGuiApplication>
#include <QClipboard>
#include <QCursor>

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

    auto topToolbar = new QHBoxLayout();

    auto title = new QLabel("Console Logs", this);
    title->setFont(QFont("sans-serif", 14, QFont::Bold));
    topToolbar->addWidget(title);

    m_logCountLabel = new QLabel("0 logs", this);
    m_logCountLabel->setStyleSheet("color: #6c6c70; margin-left: 8px;");
    topToolbar->addWidget(m_logCountLabel);

    topToolbar->addStretch();

    m_levelFilterCombo = new QComboBox(this);
    m_levelFilterCombo->addItems({"Trace & Above", "Debug & Above", "Info & Above", "Warn & Above", "Error Only"});
    m_levelFilterCombo->setCurrentIndex(0);
    connect(m_levelFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConsoleLogsView::refreshLogs);
    topToolbar->addWidget(m_levelFilterCombo);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Filter logs...");
    m_searchEdit->setFixedWidth(200);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ConsoleLogsView::refreshLogs);
    topToolbar->addWidget(m_searchEdit);

    m_copyBtn = new QPushButton("Copy Logs", this);
    connect(m_copyBtn, &QPushButton::clicked, this, &ConsoleLogsView::copyAllLogs);
    topToolbar->addWidget(m_copyBtn);

    m_clearBtn = new QPushButton("Clear Logs", this);
    connect(m_clearBtn, &QPushButton::clicked, [this]() {
        if (LogManager::instance()) LogManager::instance()->clear();
    });
    topToolbar->addWidget(m_clearBtn);

    m_autoScrollCheck = new QCheckBox("Auto-scroll", this);
    m_autoScrollCheck->setChecked(true);
    topToolbar->addWidget(m_autoScrollCheck);

    mainLayout->addLayout(topToolbar);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({"Timestamp", "Level", "Message"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_table, &QTableWidget::customContextMenuRequested, [this](const QPoint& pos) {
        Q_UNUSED(pos);
        QMenu menu(this);
        auto copySel = menu.addAction("Copy Selected Log(s)");
        connect(copySel, &QAction::triggered, this, &ConsoleLogsView::copySelectedLogs);

        auto copyAll = menu.addAction("Copy All Filtered Logs");
        connect(copyAll, &QAction::triggered, this, &ConsoleLogsView::copyAllLogs);

        menu.addSeparator();
        auto clearAct = menu.addAction("Clear All Logs");
        connect(clearAct, &QAction::triggered, [this]() {
            if (LogManager::instance()) LogManager::instance()->clear();
        });

        menu.exec(QCursor::pos());
    });

    mainLayout->addWidget(m_table);
}

void ConsoleLogsView::refreshLogs() {
    m_table->setRowCount(0);
    if (!LogManager::instance()) return;

    LogLevel filterLevel = LogLevel::Trace;
    switch (m_levelFilterCombo->currentIndex()) {
    case 0: filterLevel = LogLevel::Trace; break;
    case 1: filterLevel = LogLevel::Debug; break;
    case 2: filterLevel = LogLevel::Info; break;
    case 3: filterLevel = LogLevel::Warn; break;
    case 4: filterLevel = LogLevel::Error; break;
    }

    auto entries = LogManager::instance()->logs(filterLevel, m_searchEdit->text());
    m_logCountLabel->setText(QString("%1 logs").arg(entries.size()));

    for (const auto& entry : entries) {
        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto timeItem = new QTableWidgetItem(entry.timestamp.toString("hh:mm:ss.zzz"));
        auto levelItem = new QTableWidgetItem(logLevelToString(entry.level));
        auto msgItem = new QTableWidgetItem(entry.message);

        if (entry.level == LogLevel::Error) {
            levelItem->setForeground(QColor("#ff3b30"));
        } else if (entry.level == LogLevel::Warn) {
            levelItem->setForeground(QColor("#ff9500"));
        } else if (entry.level == LogLevel::Info) {
            levelItem->setForeground(QColor("#34c759"));
        } else {
            levelItem->setForeground(QColor("#8e8e93"));
        }

        m_table->setItem(row, 0, timeItem);
        m_table->setItem(row, 1, levelItem);
        m_table->setItem(row, 2, msgItem);
    }
    if (m_autoScrollCheck && m_autoScrollCheck->isChecked()) {
        m_table->scrollToBottom();
    }
}

void ConsoleLogsView::onLogAppended(const LogEntry& entry) {
    Q_UNUSED(entry);
    refreshLogs();
}

void ConsoleLogsView::copySelectedLogs() {
    auto ranges = m_table->selectedRanges();
    if (ranges.isEmpty()) return;

    QStringList textRows;
    for (const auto& range : ranges) {
        for (int r = range.topRow(); r <= range.bottomRow(); ++r) {
            QString time = m_table->item(r, 0) ? m_table->item(r, 0)->text() : "";
            QString lvl = m_table->item(r, 1) ? m_table->item(r, 1)->text() : "";
            QString msg = m_table->item(r, 2) ? m_table->item(r, 2)->text() : "";
            textRows.append(QString("[%1] [%2] %3").arg(time, lvl, msg));
        }
    }
    QGuiApplication::clipboard()->setText(textRows.join("\n"));
}

void ConsoleLogsView::copyAllLogs() {
    QStringList textRows;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QString time = m_table->item(r, 0) ? m_table->item(r, 0)->text() : "";
        QString lvl = m_table->item(r, 1) ? m_table->item(r, 1)->text() : "";
        QString msg = m_table->item(r, 2) ? m_table->item(r, 2)->text() : "";
        textRows.append(QString("[%1] [%2] %3").arg(time, lvl, msg));
    }
    QGuiApplication::clipboard()->setText(textRows.join("\n"));
}
