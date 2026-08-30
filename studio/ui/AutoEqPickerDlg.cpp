#include "ui/AutoEqPickerDlg.h"

#include "models/EQPreset.h" // for EQPreset

#include <QDialogButtonBox> // for QDialogButtonBox
#include <QFont>            // for QFont
#include <QFontDatabase>    // for QFontDatabase
#include <QFontMetrics>     // for QFontMetrics
#include <QFormLayout>      // for QFormLayout
#include <QLabel>           // for QLabel
#include <QLineEdit>        // for QLineEdit
#include <QList>            // for QList
#include <QListWidget>      // for QListWidget
#include <QListWidgetItem>  // for QListWidgetItem
#include <QPointer>         // for QPointer
#include <QPushButton>      // for QPushButton
#include <QStackedWidget>   // for QStackedWidget
#include <QVBoxLayout>      // for QVBoxLayout
#include <QVariant>         // for QVariant
#include <Qt>               // for AlignmentFlag, ItemDataRole, CaseSensitivity, TextElideMode
#include <algorithm>        // for min
#include <functional>       // for function
#include <optional>         // for optional
#include <stddef.h>         // for size_t
#include <string>           // for basic_string

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

    // Search header using QFormLayout
    auto searchLayout = new QFormLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search 0 headphones...");
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &AutoEqPickerDlg::onSearchTextChanged);
    searchLayout->addRow("Search:", m_searchEdit);
    mainLayout->addLayout(searchLayout);

    // Stacked widget for list, loading, error, and empty states
    m_stackedWidget = new QStackedWidget(this);

    // 1. List view
    m_listWidget = new QListWidget(m_stackedWidget);
    m_listWidget->setAlternatingRowColors(false);
    connect(m_listWidget, &QListWidget::itemClicked, this, [this]() {
        if (!m_isImporting) {
            onImportClicked();
        }
    });
    connect(m_listWidget, &QListWidget::itemActivated, this, [this]() {
        if (!m_isImporting) {
            onImportClicked();
        }
    });
    m_stackedWidget->addWidget(m_listWidget);

    // 2. Loading state
    m_loadingWidget = new QWidget(m_stackedWidget);
    auto loadingLayout = new QVBoxLayout(m_loadingWidget);
    loadingLayout->setAlignment(Qt::AlignCenter);
    m_loadingLabel = new QLabel(m_loadingWidget);
    QFont bold13 = font();
    bold13.setPointSize(13);
    bold13.setBold(true);
    m_loadingLabel->setFont(bold13);
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLayout->addWidget(m_loadingLabel);
    m_stackedWidget->addWidget(m_loadingWidget);

    // 3. Error state
    m_errorWidget = new QWidget(m_stackedWidget);
    auto errorLayout = new QVBoxLayout(m_errorWidget);
    errorLayout->setAlignment(Qt::AlignCenter);
    m_errorTitle = new QLabel("Error", m_errorWidget);
    m_errorTitle->setFont(bold13);
    m_errorTitle->setAlignment(Qt::AlignCenter);
    m_errorSubtitle = new QLabel(m_errorWidget);
    m_errorSubtitle->setWordWrap(true);
    m_errorSubtitle->setAlignment(Qt::AlignCenter);
    errorLayout->addWidget(m_errorTitle);
    errorLayout->addWidget(m_errorSubtitle);
    m_stackedWidget->addWidget(m_errorWidget);

    // 4. Empty state
    m_emptyWidget = new QWidget(m_stackedWidget);
    auto emptyLayout = new QVBoxLayout(m_emptyWidget);
    emptyLayout->setAlignment(Qt::AlignCenter);
    m_emptyTitle = new QLabel("No Results", m_emptyWidget);
    m_emptyTitle->setFont(bold13);
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptySubtitle = new QLabel("Check the spelling or try a new search.", m_emptyWidget);
    m_emptySubtitle->setWordWrap(true);
    m_emptySubtitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(m_emptyTitle);
    emptyLayout->addWidget(m_emptySubtitle);
    m_stackedWidget->addWidget(m_emptyWidget);

    mainLayout->addWidget(m_stackedWidget, 1);

    // Button box with standard layout guidelines
    m_buttonBox = new QDialogButtonBox(this);
    m_refreshBtn = m_buttonBox->addButton("Refresh", QDialogButtonBox::ActionRole);
    m_refreshBtn->setToolTip("Force refresh database from GitHub");
    connect(m_refreshBtn, &QPushButton::clicked, this, &AutoEqPickerDlg::refreshDatabase);

    m_cancelBtn = m_buttonBox->addButton(QDialogButtonBox::Cancel);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    mainLayout->addWidget(m_buttonBox);

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

    std::vector<size_t> matchedIndices;
    if (text.isEmpty()) {
        size_t limit = std::min<size_t>(m_entries.size(), 50);
        for (size_t i = 0; i < limit; ++i) {
            matchedIndices.push_back(i);
        }
    } else {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            QString name = QString::fromStdString(m_entries[i].name);
            if (name.contains(text, Qt::CaseInsensitive)) {
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
        QFont nameFont = font();
        nameFont.setBold(true);
        nameLbl->setFont(nameFont);

        auto pathLbl = new QLabel(container);
        QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        monoFont.setPointSize(9);
        pathLbl->setFont(monoFont);
        QString fullPath = QString::fromStdString(entry.path);
        pathLbl->setText(QFontMetrics(monoFont).elidedText(fullPath, Qt::ElideLeft, 440));
        pathLbl->setToolTip(fullPath);

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
    m_listWidget->setEnabled(!m_isLoading && !m_isImporting);

    if (m_isLoading) {
        m_loadingLabel->setText("Loading database...");
        m_stackedWidget->setCurrentWidget(m_loadingWidget);
    } else if (m_isImporting) {
        m_loadingLabel->setText("Importing EQ profile...");
        m_stackedWidget->setCurrentWidget(m_loadingWidget);
    } else if (!m_errorMessage.isEmpty()) {
        m_errorTitle->setText("Error ⚠️");
        m_errorSubtitle->setText(m_errorMessage);
        m_stackedWidget->setCurrentWidget(m_errorWidget);
    } else if (m_listWidget->count() == 0) {
        m_emptyTitle->setText("No Results");
        m_emptySubtitle->setText("Check the spelling or try a new search.");
        m_stackedWidget->setCurrentWidget(m_emptyWidget);
    } else {
        m_stackedWidget->setCurrentWidget(m_listWidget);
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
