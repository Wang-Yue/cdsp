#include "ui/OratoryPresetPickerDlg.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

OratoryPresetPickerDlg::OratoryPresetPickerDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Oratory1990 Headphone EQ Explorer");
    resize(600, 500);
    setupUi();
    loadIndex();
}

void OratoryPresetPickerDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    auto subtitleLbl = new QLabel("Hand-measured presets based on Oratory1990 targets", this);
    subtitleLbl->setStyleSheet("color: #8e8e93; font-size: 12px; font-weight: bold;");
    mainLayout->addWidget(subtitleLbl);

    auto searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search Oratory headphone preset...");
    connect(m_searchEdit, &QLineEdit::textChanged, this, &OratoryPresetPickerDlg::onSearchTextChanged);
    searchLayout->addWidget(m_searchEdit);

    auto refreshBtn = new QPushButton("🔄 Refresh Database", this);
    connect(refreshBtn, &QPushButton::clicked, [this]() {
        m_statusLabel->setText("Refreshing database...");
        loadIndex();
    });
    searchLayout->addWidget(refreshBtn);

    mainLayout->addLayout(searchLayout);

    m_listWidget = new QListWidget(this);
    mainLayout->addWidget(m_listWidget);

    m_statusLabel = new QLabel("Loading Oratory1990 index...", this);
    mainLayout->addWidget(m_statusLabel);

    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    m_importBtn = new QPushButton("Import Selected Preset", this);
    m_importBtn->setEnabled(false);
    connect(m_importBtn, &QPushButton::clicked, this, &OratoryPresetPickerDlg::onImportClicked);
    btnLayout->addWidget(m_importBtn);

    auto closeBtn = new QPushButton("Close", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(closeBtn);

    mainLayout->addLayout(btnLayout);

    connect(m_listWidget, &QListWidget::itemSelectionChanged, [this]() {
        m_importBtn->setEnabled(!m_listWidget->selectedItems().isEmpty());
    });
}

void OratoryPresetPickerDlg::loadIndex() {
    m_service.fetchIndex([this](bool ok, const std::vector<OratoryIndexEntry>& entries) {
        if (ok) {
            m_entries = entries;
            m_statusLabel->setText(QString("Loaded %1 Oratory presets.").arg(entries.size()));
            onSearchTextChanged(m_searchEdit->text());
        } else {
            m_statusLabel->setText("Failed to load Oratory index.");
        }
    });
}

void OratoryPresetPickerDlg::onSearchTextChanged(const QString& text) {
    m_listWidget->clear();
    for (size_t i = 0; i < m_entries.size(); ++i) {
        QString name = QString::fromStdString(m_entries[i].name);
        if (text.isEmpty() || name.contains(text, Qt::CaseInsensitive)) {
            auto item = new QListWidgetItem(name, m_listWidget);
            item->setData(Qt::UserRole, static_cast<int>(i));
        }
    }
}

void OratoryPresetPickerDlg::onImportClicked() {
    auto items = m_listWidget->selectedItems();
    if (items.isEmpty()) return;

    int idx = items[0]->data(Qt::UserRole).toInt();
    const auto& entry = m_entries[idx];

    m_statusLabel->setText("Downloading preset...");
    m_service.fetchPreset(entry, [this, entry](bool ok, std::optional<EQPreset> preset) {
        if (ok && preset.has_value()) {
            m_pipeline->addEQPreset(preset.value());
            QMessageBox::information(this, "Success", QString("Imported preset '%1'").arg(QString::fromStdString(entry.name)));
            accept();
        } else {
            m_statusLabel->setText("Failed to download preset.");
        }
    });
}
