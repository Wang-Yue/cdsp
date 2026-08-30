#ifndef CONVOLUTION_IMPORT_DLG_H
#define CONVOLUTION_IMPORT_DLG_H

#include "models/PipelineStore.h" // for PipelineStore

#include <QDialog>        // for QDialog
#include <QFrame>         // for QFrame
#include <QFutureWatcher> // for QFutureWatcher
#include <QLabel>         // for QLabel
#include <QObject>        // for Q_OBJECT, slots
#include <QPushButton>    // for QPushButton
#include <QString>        // for QString
#include <QStringList>    // for QStringList
#include <QWidget>        // for QWidget
#include <map>            // for map
#include <memory>         // for shared_ptr
#include <string>         // for basic_string, string
#include <vector>         // for vector

class QDialogButtonBox;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

class ConvolutionImportDlg : public QDialog {
    Q_OBJECT

public:
    ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr);
    ~ConvolutionImportDlg() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onAddFilesClicked();
    void onImportClicked();
    void onImportFinished();
    void updateItemsList();

private:
    std::shared_ptr<PipelineStore> m_pipeline;

    struct ImportItem {
        QString filePath;
        int sampleRate = 48000;
        QString format = "WAV";
        int channel = 0;
        bool invertPhase = false;
    };

    struct ImportResult {
        bool success = false;
        QString errorMessage;
        std::map<int, std::string> paths;
        int firstCoeffCount = 0;
    };

    std::vector<ImportItem> m_items;

    QLineEdit* m_nameEdit = nullptr;
    QLineEdit* m_kindEdit = nullptr;
    QVBoxLayout* m_itemListLayout = nullptr;
    QFrame* m_emptyStateWidget = nullptr;
    QLabel* m_warningLabel = nullptr;
    QFrame* m_errorWidget = nullptr;
    QLabel* m_errorLabel = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;
    QPushButton* m_importBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QScrollArea* m_scrollArea = nullptr;

    bool m_isImporting = false;
    QFutureWatcher<ImportResult> m_watcher;

    void setupUi();
    void addImportFiles(const QStringList& files);
    bool hasDuplicateRates() const;
    void updateImportButtonState();
};

#endif // CONVOLUTION_IMPORT_DLG_H
