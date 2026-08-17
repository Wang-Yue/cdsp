#include "ui/ConsoleLogsView.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QFrame>
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
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 1. Top Toolbar Header Container
    m_headerWidget = new QWidget(this);

    auto topToolbar = new QHBoxLayout(m_headerWidget);
    topToolbar->setContentsMargins(16, 16, 16, 16);
    topToolbar->setSpacing(12);

    auto title = new QLabel("Console Logs", m_headerWidget);
    QFont titleFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    titleFont.setPointSize(14);
    titleFont.setWeight(QFont::Bold);
    title->setFont(titleFont);
    topToolbar->addWidget(title);

    topToolbar->addStretch();

    m_levelFilterCombo = new QComboBox(m_headerWidget);
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

    m_copyBtn = new QPushButton("Copy", m_headerWidget);
    connect(m_copyBtn, &QPushButton::clicked, this, &ConsoleLogsView::copyAllLogs);
    topToolbar->addWidget(m_copyBtn);

    m_clearBtn = new QPushButton("Clear", m_headerWidget);
    connect(m_clearBtn, &QPushButton::clicked, [this]() {
        if (LogManager::instance())
            LogManager::instance()->clear();
    });
    topToolbar->addWidget(m_clearBtn);

    m_autoScrollCheck = new QCheckBox("Auto-scroll", m_headerWidget);
    m_autoScrollCheck->setChecked(true);
    topToolbar->addWidget(m_autoScrollCheck);

    mainLayout->addWidget(m_headerWidget);

    // 2. Divider Line
    auto divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(divider);

    // 3. Scroll Container / Log Table Area
    auto scrollContainer = new QWidget(this);
    auto scrollLayout = new QVBoxLayout(scrollContainer);
    scrollLayout->setContentsMargins(16, 16, 16, 16);
    scrollLayout->setSpacing(0);

    m_table = new QTableWidget(scrollContainer);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"Timestamp", "Message"});
    m_table->horizontalHeader()->setVisible(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 70);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setShowGrid(false);
    m_table->setWordWrap(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);

    scrollLayout->addWidget(m_table);
    mainLayout->addWidget(scrollContainer);
}

void ConsoleLogsView::refreshLogs() {
    m_table->setRowCount(0);
    if (!LogManager::instance())
        return;

    auto captionMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    captionMono.setPointSize(11);

    auto bodyMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    bodyMono.setPointSize(13);

    QColor secondaryColor = palette().color(QPalette::PlaceholderText);

    auto entries = LogManager::instance()->logs();
    for (const auto& entry : entries) {
        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss"));
        timeItem->setFont(captionMono);
        timeItem->setForeground(secondaryColor);
        timeItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

        auto msgItem = new QTableWidgetItem(entry.message);
        msgItem->setFont(bodyMono);
        msgItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

        m_table->setItem(row, 0, timeItem);
        m_table->setItem(row, 1, msgItem);
    }
    if (m_autoScrollCheck && m_autoScrollCheck->isChecked()) {
        m_table->scrollToBottom();
    }
}

void ConsoleLogsView::onLogAppended(const LogEntry& entry) {
    int row = m_table->rowCount();
    m_table->insertRow(row);

    auto captionMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    captionMono.setPointSize(11);

    auto bodyMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    bodyMono.setPointSize(13);

    auto timeItem = new QTableWidgetItem(entry.timestamp.toString("HH:mm:ss"));
    timeItem->setFont(captionMono);
    timeItem->setForeground(palette().color(QPalette::PlaceholderText));
    timeItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    auto msgItem = new QTableWidgetItem(entry.message);
    msgItem->setFont(bodyMono);
    msgItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_table->setItem(row, 0, timeItem);
    m_table->setItem(row, 1, msgItem);

    constexpr int kMaxTableRows = 2000;
    while (m_table->rowCount() > kMaxTableRows) {
        m_table->removeRow(0);
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
        textRows.append(QString("[%1] %2").arg(entry.timestamp.toString(Qt::ISODateWithMs), entry.message));
    }
    QGuiApplication::clipboard()->setText(textRows.join("\n"));
}
