#include "ui/AutoEqPickerDlg.h"

#include "ui/StyleTheme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPointer>
#include <QVBoxLayout>
#include <algorithm>

AutoEqPickerDlg::AutoEqPickerDlg(std::shared_ptr<PipelineStore> pipeline,
                                 std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline), m_dspController(dspController) {
    setWindowTitle("AutoEQ Database");
    resize(500, 600);
    setupUi();
    loadDatabase(false);
}

void AutoEqPickerDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top Header: HStack(spacing: 16) with padding 16
    auto headerWidget = new QWidget(this);
    auto headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 16, 16, 16);
    headerLayout->setSpacing(16);

    auto headerTitle = new QLabel("AutoEQ Database", headerWidget);
    headerTitle->setFont(QFont("sans-serif", 13, QFont::Bold));
    headerTitle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    headerLayout->addWidget(headerTitle);

    m_searchEdit = new QLineEdit(headerWidget);
    m_searchEdit->setPlaceholderText("Search 0 headphones...");
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &AutoEqPickerDlg::onSearchTextChanged);
    headerLayout->addWidget(m_searchEdit);

    mainLayout->addWidget(headerWidget);

    // Divider
    auto divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    divider->setStyleSheet("color: #d1d1d6;");
    mainLayout->addWidget(divider);

    // List and Overlay Stack
    auto stackWidget = new QWidget(this);
    auto gridLayout = new QGridLayout(stackWidget);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(0);

    m_listWidget = new QListWidget(stackWidget);
    m_listWidget->setAlternatingRowColors(false);
    connect(m_listWidget, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
        if (!m_isImporting) {
            onImportClicked();
        }
    });
    connect(m_listWidget, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
        if (!m_isImporting) {
            onImportClicked();
        }
    });
    gridLayout->addWidget(m_listWidget, 0, 0);

    // Overlay Widget
    m_overlayWidget = new QWidget(stackWidget);
    m_overlayWidget->setAutoFillBackground(true);
    auto overlayLayout = new QVBoxLayout(m_overlayWidget);
    overlayLayout->setAlignment(Qt::AlignCenter);
    overlayLayout->setSpacing(8);

    m_overlayTitle = new QLabel(m_overlayWidget);
    m_overlayTitle->setFont(QFont("sans-serif", 13, QFont::Bold));
    m_overlayTitle->setAlignment(Qt::AlignCenter);

    m_overlaySubtitle = new QLabel(m_overlayWidget);
    m_overlaySubtitle->setFont(QFont("sans-serif", 11));
    m_overlaySubtitle->setStyleSheet("color: #8e8e93;");
    m_overlaySubtitle->setWordWrap(true);
    m_overlaySubtitle->setAlignment(Qt::AlignCenter);

    overlayLayout->addWidget(m_overlayTitle);
    overlayLayout->addWidget(m_overlaySubtitle);
    gridLayout->addWidget(m_overlayWidget, 0, 0);

    mainLayout->addWidget(stackWidget, 1);

    // Bottom Toolbar / Footer
    auto toolbarWidget = new QWidget(this);
    auto toolbarLayout = new QHBoxLayout(toolbarWidget);
    toolbarLayout->setContentsMargins(16, 12, 16, 12);

    m_cancelBtn = new QPushButton("Cancel", toolbarWidget);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    toolbarLayout->addWidget(m_cancelBtn);

    toolbarLayout->addStretch();

    m_refreshBtn = new QPushButton("Refresh", toolbarWidget);
    m_refreshBtn->setToolTip("Force refresh database from GitHub");
    connect(m_refreshBtn, &QPushButton::clicked, this, &AutoEqPickerDlg::refreshDatabase);
    toolbarLayout->addWidget(m_refreshBtn);

    mainLayout->addWidget(toolbarWidget);

    updateUiState();
}

void AutoEqPickerDlg::loadDatabase(bool forceRefresh) {
    m_isLoading = true;
    m_errorMessage.clear();
    updateUiState();

    QPointer<AutoEqPickerDlg> safeThis(this);
    m_service.fetchIndex(
        [safeThis](bool ok, const std::vector<AutoEqIndexEntry>& entries) {
            if (!safeThis)
                return;
            safeThis->m_isLoading = false;
            if (ok) {
                safeThis->m_entries = entries;
                safeThis->m_searchEdit->setPlaceholderText(QString("Search %1 headphones...").arg(entries.size()));
                safeThis->onSearchTextChanged(safeThis->m_searchEdit->text());
            } else {
                safeThis->m_errorMessage = "Failed to fetch database: Network or parsing error";
                safeThis->updateUiState();
            }
        },
        forceRefresh);
}

void AutoEqPickerDlg::refreshDatabase() {
    loadDatabase(true);
}

void AutoEqPickerDlg::onSearchTextChanged(const QString& text) {
    m_listWidget->clear();
    QString trimmed = text.trimmed();

    std::vector<size_t> matchedIndices;
    if (trimmed.isEmpty()) {
        size_t limit = std::min<size_t>(m_entries.size(), 50);
        for (size_t i = 0; i < limit; ++i) {
            matchedIndices.push_back(i);
        }
    } else {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            QString name = QString::fromStdString(m_entries[i].name);
            if (name.contains(trimmed, Qt::CaseInsensitive)) {
                matchedIndices.push_back(i);
            }
        }
    }

    for (size_t idx : matchedIndices) {
        const auto& entry = m_entries[idx];
        auto item = new QListWidgetItem(m_listWidget);
        item->setData(Qt::UserRole, static_cast<int>(idx));

        auto container = new QWidget();
        auto itemLayout = new QVBoxLayout(container);
        itemLayout->setContentsMargins(8, 4, 8, 4);
        itemLayout->setSpacing(2);

        auto nameLbl = new QLabel(QString::fromStdString(entry.name), container);
        nameLbl->setFont(QFont("sans-serif", 11, QFont::Bold));

        auto pathLbl = new QLabel(QString::fromStdString(entry.path), container);
        QFont monoFont("monospace", 9);
        monoFont.setStyleHint(QFont::Monospace);
        pathLbl->setFont(monoFont);
        pathLbl->setStyleSheet("color: #8e8e93;");

        itemLayout->addWidget(nameLbl);
        itemLayout->addWidget(pathLbl);

        item->setSizeHint(container->sizeHint());
        m_listWidget->addItem(item);
        m_listWidget->setItemWidget(item, container);
    }

    updateUiState();
}

void AutoEqPickerDlg::updateUiState() {
    m_refreshBtn->setEnabled(!m_isLoading && !m_isImporting);
    m_searchEdit->setEnabled(!m_isLoading && !m_isImporting);

    if (m_isLoading) {
        m_overlayWidget->show();
        m_overlayWidget->raise();
        m_overlayWidget->setStyleSheet("background-color: transparent;");
        m_overlayTitle->setText("Loading database...");
        m_overlaySubtitle->setText("");
    } else if (m_isImporting) {
        m_overlayWidget->show();
        m_overlayWidget->raise();
        m_overlayWidget->setStyleSheet("background-color: rgba(240, 240, 240, 0.75);");
        m_overlayTitle->setText("Importing EQ profile...");
        m_overlaySubtitle->setText("");
    } else if (!m_errorMessage.isEmpty()) {
        m_overlayWidget->show();
        m_overlayWidget->raise();
        m_overlayWidget->setStyleSheet("background-color: transparent;");
        m_overlayTitle->setText("Error ⚠️");
        m_overlaySubtitle->setText(m_errorMessage);
    } else if (m_listWidget->count() == 0) {
        m_overlayWidget->show();
        m_overlayWidget->raise();
        m_overlayWidget->setStyleSheet("background-color: transparent;");
        m_overlayTitle->setText("No Results");
        m_overlaySubtitle->setText("Check the spelling or try a new search.");
    } else {
        m_overlayWidget->hide();
    }
}

void AutoEqPickerDlg::onImportClicked() {
    if (m_isImporting)
        return;

    auto items = m_listWidget->selectedItems();
    if (items.isEmpty())
        return;

    int idx = items[0]->data(Qt::UserRole).toInt();
    const auto& entry = m_entries[idx];

    m_isImporting = true;
    m_errorMessage.clear();
    updateUiState();

    QPointer<AutoEqPickerDlg> safeThis(this);
    m_service.fetchPreset(entry, [safeThis, entry](bool ok, std::optional<EQPreset> preset) {
        if (!safeThis)
            return;

        if (ok && preset.has_value()) {
            auto p = preset.value();
            p.name = entry.name;
            safeThis->m_pipeline->addEQPreset(p);
            safeThis->accept();
        } else {
            safeThis->m_isImporting = false;
            safeThis->m_errorMessage = "Could not parse EQ data. File format might have changed.";
            safeThis->updateUiState();
        }
    });
}
