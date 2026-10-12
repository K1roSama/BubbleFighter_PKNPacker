#pragma once
#include "ArchiveWorkspace.h"
#include <QMainWindow>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QStringList>
#include <functional>
#include <optional>
#include <thread>

class QTreeWidget;
class QTableView;
class QTableWidget;
class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QProgressBar;
class QLabel;
class QPushButton;
class EntryModel;
class EntryProxy;

struct ManagedPackage {
    std::shared_ptr<PKNPacker::ArchiveCatalog> archive;
    std::shared_ptr<PKNPacker::ArchiveWorkspace> workspace;
};
struct EntryRow {
    std::size_t package = 0;
    PKNPacker::ArchiveEntry entry;
    std::optional<PKNPacker::WorkspaceEntry> staged;
};
struct GuiTask {
    QString label;
    std::function<void(const PKNPacker::ArchiveJob&)> action;
    std::shared_ptr<PKNPacker::JobResult> details;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void OpenPaths(const QStringList& paths);
    void OpenWorkspaces(const QStringList& paths);
    void ConfigureSmokeTest(QString screenshot, QString report);
private:
    void RunTasks(const QString& title, std::vector<GuiTask> tasks,
                  std::function<void(const PKNPacker::JobResult&)> complete = {});
    void RebuildCatalog();
    void SetScopes();
    void Preview(std::uint64_t offset = 0);
    void ExportSelected(bool singleFile);
    void ExtractPackages(bool selectionOnly = false, std::function<void(const PKNPacker::JobResult&)> complete = {});
    void PackWorkspaces();
    void StageReplace();
    void StageAdd();
    void StageDelete(bool undo);
    void CheckChanges();
    void VerifyPackages();
    void CreatePackage();
    void EnsureWorkspace(std::size_t package, std::function<void(std::shared_ptr<PKNPacker::ArchiveWorkspace>)> complete);
    void ShowChanges(const std::vector<std::pair<QString, PKNPacker::ChangeSummary>>& changes, std::function<void()> build = {});
    void ShowError(const QString& message);
    std::vector<EntryRow> SelectedRows() const;
    std::vector<std::size_t> SelectedPackages() const;
    std::filesystem::path DefaultWorkspaceRoot() const;
    void RunSmokeTest();
    void RecordSmokeStage(const QString& stage);
    std::vector<ManagedPackage> packages_;
    EntryModel* model_ = nullptr;
    EntryProxy* proxy_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTableView* table_ = nullptr;
    QTableWidget* results_ = nullptr;
    QPlainTextEdit* textPreview_ = nullptr;
    QPlainTextEdit* hexPreview_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* extensions_ = nullptr;
    QComboBox* scopeMode_ = nullptr;
    QProgressBar* overall_ = nullptr;
    QProgressBar* current_ = nullptr;
    QLabel* information_ = nullptr;
    QLabel* previewInfo_ = nullptr;
    QLabel* taskInfo_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* previousPage_ = nullptr;
    QPushButton* nextPage_ = nullptr;
    std::vector<QAction*> mutationActions_;
    std::vector<GuiTask> failedTasks_;
    std::vector<GuiTask> finishedTasks_;
    std::function<void(const PKNPacker::JobResult&)> failedComplete_;
    PKNPacker::ArchiveJob job_;
    std::jthread worker_;
    QThreadPool previewPool_;
    std::shared_ptr<std::atomic_bool> previewCancelled_;
    std::uint64_t previewOffset_ = 0, previewGeneration_ = 0;
    bool busy_ = false;
    QString smokeScreenshot_, smokeReport_;
    QElapsedTimer smokeClock_;
    std::vector<std::pair<QString, qint64>> smokeTimings_;
};
