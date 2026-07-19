#include "ui/ConsoleLogsView.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
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
    mainLayout->setSpacing(0);

    auto topToolbar = new QHBoxLayout();
    topToolbar->setContentsMargins(0, 0, 0, 12);

    auto title = new QLabel("Console Logs", this);
    title->setFont(QFont("sans-serif", 13, QFont::Bold));
    topToolbar->addWidget(title);

    topToolbar->addStretch();

    m_levelFilterCombo = new QComboBox(this);
    m_levelFilterCombo->addItem("Off", static_cast<int>(LogLevel::Off));
    m_levelFilterCombo->addItem("Error", static_cast<int>(LogLevel::Error));
    m_levelFilterCombo->addItem("Warn", static_cast<int>(LogLevel::Warn));
    m_levelFilterCombo->addItem("Info", static_cast<int>(LogLevel::Info));
    m_levelFilterCombo->addItem("Debug", static_cast<int>(LogLevel::Debug));
    m_levelFilterCombo->addItem("Trace", static_cast<int>(LogLevel::Trace));
    m_levelFilterCombo->setFixedWidth(150);

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
    });
    topToolbar->addWidget(m_levelFilterCombo);

    m_copyBtn = new QPushButton("Copy", this);
    connect(m_copyBtn, &QPushButton::clicked, this, &ConsoleLogsView::copyAllLogs);
    topToolbar->addWidget(m_copyBtn);

    m_clearBtn = new QPushButton("Clear", this);
    connect(m_clearBtn, &QPushButton::clicked, [this]() {
        if (LogManager::instance())
            LogManager::instance()->clear();
    });
    topToolbar->addWidget(m_clearBtn);

    m_autoScrollCheck = new QCheckBox("Auto-scroll", this);
    m_autoScrollCheck->setChecked(true);
    topToolbar->addWidget(m_autoScrollCheck);

    mainLayout->addLayout(topToolbar);

    m_table = new QTableWidget(this);
    m_table->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"Timestamp", "Message"});
    m_table->horizontalHeader()->setVisible(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 70);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setShowGrid(false);
    m_table->setWordWrap(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);

    mainLayout->addWidget(m_table);
}

void ConsoleLogsView::refreshLogs() {
    m_table->setRowCount(0);
    if (!LogManager::instance())
        return;

    auto entries = LogManager::instance()->logs();
    for (const auto& entry : entries) {
        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss"));
        timeItem->setForeground(QColor("#8e8e93"));
        timeItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

        auto msgItem = new QTableWidgetItem(entry.message);
        msgItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

        m_table->setItem(row, 0, timeItem);
        m_table->setItem(row, 1, msgItem);
    }
    m_table->resizeRowsToContents();
    if (m_autoScrollCheck && m_autoScrollCheck->isChecked()) {
        m_table->scrollToBottom();
    }
}

void ConsoleLogsView::onLogAppended(const LogEntry& entry) {
    int row = m_table->rowCount();
    m_table->insertRow(row);

    auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss"));
    timeItem->setForeground(QColor("#8e8e93"));
    timeItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    auto msgItem = new QTableWidgetItem(entry.message);
    msgItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_table->setItem(row, 0, timeItem);
    m_table->setItem(row, 1, msgItem);

    constexpr int kMaxTableRows = 2000;
    while (m_table->rowCount() > kMaxTableRows) {
        m_table->removeRow(0);
    }
    m_table->resizeRowsToContents();

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
        textRows.append(QString("[%1] %2").arg(entry.timestamp.toString(Qt::ISODateWithMs), entry.message));
    }
    QGuiApplication::clipboard()->setText(textRows.join("\n"));
}
