#include "ui/ConsoleLogsView.h"

#include "models/LogManager.h" // for LogLevel, LogManager

#include <QAbstractItemModel>   // for QAbstractItemModel
#include <QAbstractItemView>    // for QAbstractItemView
#include <QAction>              // for QAction
#include <QApplication>         // for QApplication
#include <QClipboard>           // for QClipboard
#include <QColor>               // for QColor, operator<<, operator>>
#include <QCursor>              // for QCursor
#include <QFlags>               // for QFlags
#include <QFont>                // for QFont
#include <QFontDatabase>        // for QFontDatabase
#include <QGuiApplication>      // for QGuiApplication
#include <QHBoxLayout>          // for QHBoxLayout
#include <QHeaderView>          // for QHeaderView
#include <QItemSelectionModel>  // for QItemSelectionModel
#include <QMenu>                // for QMenu
#include <QModelIndex>          // for QModelIndex
#include <QPainter>             // for QPainter
#include <QPalette>             // for QPalette
#include <QPoint>               // for QPoint
#include <QRect>                // for QRect
#include <QRectF>               // for QRectF
#include <QSize>                // for QSize
#include <QString>              // for QString
#include <QStyle>               // for QStyle
#include <QStyleOptionViewItem> // for QStyleOptionViewItem
#include <QStyledItemDelegate>  // for QStyledItemDelegate
#include <QTableView>           // for QTableView
#include <QTextLayout>          // for QTextLayout, QTextLine
#include <QTextOption>          // for QTextOption
#include <QVBoxLayout>          // for QVBoxLayout
#include <QVariant>             // for QVariant
#include <Qt>                   // for AlignmentFlag, ContextMenuPolicy, ItemDataRole, TextElideMode, operator|
#include <QtGlobal>             // for QOverload, Q_UNUSED, qreal
#include <algorithm>            // for max, sort
#include <cmath>                // for ceil
#include <vector>               // for vector

namespace {

class LogMessageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

        painter->save();
        painter->setFont(opt.font);
        if (opt.state & QStyle::State_Selected) {
            painter->setPen(opt.palette.color(QPalette::HighlightedText));
        } else {
            QVariant fg = index.data(Qt::ForegroundRole);
            if (fg.isValid()) {
                painter->setPen(fg.value<QColor>());
            } else {
                painter->setPen(opt.palette.color(QPalette::Text));
            }
        }

        QRectF textRect = opt.rect.adjusted(6, 4, -6, -4);
        QTextOption to;
        to.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        to.setAlignment(Qt::AlignTop | Qt::AlignLeft);
        painter->drawText(textRect, opt.text, to);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        int colWidth = 300;
        if (const auto* view = qobject_cast<const QTableView*>(opt.widget)) {
            int w = view->columnWidth(index.column());
            if (w > 40) {
                colWidth = w;
            } else if (view->viewport()) {
                int vpW = view->viewport()->width();
                int otherCols = view->columnWidth(0) + view->columnWidth(1);
                if (vpW > otherCols + 40) {
                    colWidth = vpW - otherCols;
                }
            }
        }
        int availableWidth = std::max(40, colWidth - 14);

        QTextLayout textLayout(opt.text, opt.font);
        QTextOption to;
        to.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        textLayout.setTextOption(to);
        textLayout.beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = textLayout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(availableWidth);
            height += line.height();
        }
        textLayout.endLayout();

        int h = std::max(24, static_cast<int>(std::ceil(height)) + 8);
        return QSize(colWidth, h);
    }
};

} // namespace

ConsoleLogsView::ConsoleLogsView(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void ConsoleLogsView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_table) {
        m_table->resizeRowsToContents();
        if (m_autoScrollCheck && m_autoScrollCheck->isChecked() && m_model && m_model->rowCount() > 0) {
            m_table->scrollToBottom();
        }
    }
}

void ConsoleLogsView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_table) {
        m_table->resizeRowsToContents();
    }
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
    m_searchEdit->setMinimumWidth(100);
    m_searchEdit->setMaximumWidth(220);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_model) {
            m_model->setFilterText(text.trimmed());
            updateCountLabel();
        }
    });
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
    m_levelFilterCombo->setMinimumWidth(80);

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
        if (m_model) {
            m_model->refresh();
            updateCountLabel();
        }
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
    connect(m_clearBtn, &QPushButton::clicked, []() {
        if (LogManager::instance()) {
            LogManager::instance()->clear();
        }
    });
    toolbarLayout->addWidget(m_clearBtn);

    mainLayout->addLayout(toolbarLayout);

    // 2. Virtual Table View
    m_model = new LogTableModel(this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setItemDelegateForColumn(2, new LogMessageDelegate(m_table));
    m_table->setWordWrap(true);
    m_table->setTextElideMode(Qt::ElideNone);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setVisible(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this, [this](int logicalIndex, int, int) {
        if (logicalIndex == 2 && m_table) {
            m_table->resizeRowsToContents();
        }
    });

    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this]() {
        updateCountLabel();
        if (m_autoScrollCheck && m_autoScrollCheck->isChecked() && m_model->rowCount() > 0) {
            m_table->scrollToBottom();
        }
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        updateCountLabel();
        if (m_autoScrollCheck && m_autoScrollCheck->isChecked() && m_model->rowCount() > 0) {
            m_table->scrollToBottom();
        }
    });

    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
        Q_UNUSED(pos);
        QMenu menu(this);
        auto copySel = menu.addAction("Copy Selected");
        connect(copySel, &QAction::triggered, this, [this]() {
            if (!m_model || !m_table->selectionModel())
                return;
            auto selectedIndexes = m_table->selectionModel()->selectedRows();
            if (selectedIndexes.isEmpty())
                return;

            std::vector<int> rows;
            rows.reserve(selectedIndexes.size());
            for (const auto& idx : selectedIndexes) {
                rows.push_back(idx.row());
            }
            std::sort(rows.begin(), rows.end());
            QGuiApplication::clipboard()->setText(m_model->copySelectedFormatted(rows));
        });

        auto copyAll = menu.addAction("Copy All Logs");
        connect(copyAll, &QAction::triggered, this, &ConsoleLogsView::copyAllLogs);

        menu.addSeparator();
        auto clearAct = menu.addAction("Clear Logs");
        connect(clearAct, &QAction::triggered, this, []() {
            if (LogManager::instance()) {
                LogManager::instance()->clear();
            }
        });

        menu.exec(QCursor::pos());
    });

    mainLayout->addWidget(m_table);
    updateCountLabel();
}

void ConsoleLogsView::updateCountLabel() {
    if (m_logCountLabel && m_model) {
        m_logCountLabel->setText(QString("%1 logs").arg(m_model->rowCount()));
    }
}

void ConsoleLogsView::copyAllLogs() {
    if (!m_model)
        return;
    QGuiApplication::clipboard()->setText(m_model->copyAllFormatted());
}
