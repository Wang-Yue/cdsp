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
    m_levelFilterCombo->addItem("Trace & Above", static_cast<int>(LogLevel::Trace));
    m_levelFilterCombo->addItem("Debug & Above", static_cast<int>(LogLevel::Debug));
    m_levelFilterCombo->addItem("Info & Above", static_cast<int>(LogLevel::Info));
    m_levelFilterCombo->addItem("Warn & Above", static_cast<int>(LogLevel::Warn));
    m_levelFilterCombo->addItem("Error Only", static_cast<int>(LogLevel::Error));
    m_levelFilterCombo->setCurrentIndex(0);
    connect(m_levelFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConsoleLogsView::refreshLogs);
    topToolbar->addWidget(m_levelFilterCombo);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Filter logs...");
    m_searchEdit->setClearButtonEnabled(true);
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

    LogLevel filterLevel = static_cast<LogLevel>(m_levelFilterCombo->currentData().toInt());

    auto entries = LogManager::instance()->logs(filterLevel, m_searchEdit->text());
    m_logCountLabel->setText(QString("%1 logs").arg(entries.size()));

    for (const auto& entry : entries) {
        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss.zzz"));
        auto levelItem = new QTableWidgetItem(logLevelToString(entry.level));
        auto msgItem = new QTableWidgetItem(entry.message);

        switch (entry.level) {
        case LogLevel::Error:
            levelItem->setForeground(QColor("#ff3b30")); // Red
            break;
        case LogLevel::Warn:
            levelItem->setForeground(QColor("#ff9500")); // Orange
            break;
        case LogLevel::Info:
            levelItem->setForeground(QColor("#34c759")); // Green
            break;
        case LogLevel::Debug:
            levelItem->setForeground(QColor("#007aff")); // Blue
            break;
        case LogLevel::Trace:
        default:
            levelItem->setForeground(QColor("#8e8e93")); // Gray
            break;
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
    LogLevel filterLevel = static_cast<LogLevel>(m_levelFilterCombo->currentData().toInt());
    if (static_cast<int>(entry.level) < static_cast<int>(filterLevel)) return;

    QString search = m_searchEdit->text();
    if (!search.isEmpty() && !entry.message.contains(search, Qt::CaseInsensitive)) return;

    int row = m_table->rowCount();
    m_table->insertRow(row);

    auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss.zzz"));
    auto levelItem = new QTableWidgetItem(logLevelToString(entry.level));
    auto msgItem = new QTableWidgetItem(entry.message);

    switch (entry.level) {
    case LogLevel::Error:
        levelItem->setForeground(QColor("#ff3b30"));
        break;
    case LogLevel::Warn:
        levelItem->setForeground(QColor("#ff9500"));
        break;
    case LogLevel::Info:
        levelItem->setForeground(QColor("#34c759"));
        break;
    case LogLevel::Debug:
        levelItem->setForeground(QColor("#007aff"));
        break;
    case LogLevel::Trace:
    default:
        levelItem->setForeground(QColor("#8e8e93"));
        break;
    }

    m_table->setItem(row, 0, timeItem);
    m_table->setItem(row, 1, levelItem);
    m_table->setItem(row, 2, msgItem);

    m_logCountLabel->setText(QString("%1 logs").arg(m_table->rowCount()));

    if (m_autoScrollCheck && m_autoScrollCheck->isChecked()) {
        m_table->scrollToBottom();
    }
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
