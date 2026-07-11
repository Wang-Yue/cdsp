#include "ui/AutoEqPickerDlg.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

AutoEqPickerDlg::AutoEqPickerDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("AutoEQ Online Preset Explorer");
    resize(620, 520);
    setupUi();
    loadIndex();
}

void AutoEqPickerDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    auto headerTitle = new QLabel("AutoEQ Database", this);
    headerTitle->setFont(QFont("sans-serif", 13, QFont::Bold));
    mainLayout->addWidget(headerTitle);

    auto searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search headphone model...");
    connect(m_searchEdit, &QLineEdit::textChanged, this, &AutoEqPickerDlg::onSearchTextChanged);
    searchLayout->addWidget(m_searchEdit);

    auto refreshBtn = new QPushButton("🔄 Refresh Database", this);
    connect(refreshBtn, &QPushButton::clicked, [this]() {
        m_statusLabel->setText("Refreshing database from GitHub...");
        loadIndex();
    });
    searchLayout->addWidget(refreshBtn);

    mainLayout->addLayout(searchLayout);

    m_listWidget = new QListWidget(this);
    m_listWidget->setAlternatingRowColors(true);
    connect(m_listWidget, &QListWidget::itemDoubleClicked, this, &AutoEqPickerDlg::onImportClicked);
    mainLayout->addWidget(m_listWidget);

    m_statusLabel = new QLabel("Loading index from GitHub...", this);
    m_statusLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    mainLayout->addWidget(m_statusLabel);

    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    m_importBtn = new QPushButton("Import Selected Preset", this);
    m_importBtn->setStyleSheet("background-color: #007af5; color: white; font-weight: bold; padding: 5px 14px; border-radius: 4px;");
    m_importBtn->setEnabled(false);
    connect(m_importBtn, &QPushButton::clicked, this, &AutoEqPickerDlg::onImportClicked);
    btnLayout->addWidget(m_importBtn);

    auto closeBtn = new QPushButton("Cancel", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(closeBtn);

    mainLayout->addLayout(btnLayout);

    connect(m_listWidget, &QListWidget::itemSelectionChanged, [this]() {
        m_importBtn->setEnabled(!m_listWidget->selectedItems().isEmpty());
    });
}

void AutoEqPickerDlg::loadIndex() {
    m_service.fetchIndex([this](bool ok, const std::vector<AutoEqIndexEntry>& entries) {
        if (ok) {
            m_entries = entries;
            m_statusLabel->setText(QString("Loaded %1 headphone presets from GitHub.").arg(entries.size()));
            m_searchEdit->setPlaceholderText(QString("Search %1 headphones...").arg(entries.size()));
            onSearchTextChanged(m_searchEdit->text());
        } else {
            m_statusLabel->setText("Failed to load AutoEQ index. Check internet connection.");
        }
    });
}

void AutoEqPickerDlg::onSearchTextChanged(const QString& text) {
    m_listWidget->clear();
    int count = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        QString name = QString::fromStdString(m_entries[i].name);
        QString path = QString::fromStdString(m_entries[i].path);
        if (text.isEmpty() || name.contains(text, Qt::CaseInsensitive) || path.contains(text, Qt::CaseInsensitive)) {
            auto item = new QListWidgetItem(m_listWidget);
            item->setText(QString("%1\n%2").arg(name, path));
            item->setData(Qt::UserRole, static_cast<int>(i));
            count++;
            if (text.isEmpty() && count >= 100) break; // limit initial view for responsiveness
        }
    }
}

void AutoEqPickerDlg::onImportClicked() {
    auto items = m_listWidget->selectedItems();
    if (items.isEmpty()) return;

    int idx = items[0]->data(Qt::UserRole).toInt();
    const auto& entry = m_entries[idx];

    m_statusLabel->setText("Downloading preset...");
    m_importBtn->setEnabled(false);

    m_service.fetchPreset(entry, [this, entry](bool ok, std::optional<EQPreset> preset) {
        m_importBtn->setEnabled(true);
        if (ok && preset.has_value()) {
            auto p = preset.value();
            p.name = entry.name;
            m_pipeline->addEQPreset(p);
            QMessageBox::information(this, "Success", QString("Imported preset '%1' into Pipeline Store.").arg(QString::fromStdString(entry.name)));
            accept();
        } else {
            m_statusLabel->setText("Failed to download or parse preset file.");
        }
    });
}

