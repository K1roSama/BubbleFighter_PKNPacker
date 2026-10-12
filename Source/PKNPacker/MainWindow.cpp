#include "MainWindow.h"
#include "Json.h"
#include "ui_ManagerShell.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QStringDecoder>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace PKNPacker;
namespace {
QString Q(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string U(const QString& text) { const auto b = text.toUtf8(); return {b.constData(), static_cast<std::size_t>(b.size())}; }
std::filesystem::path P(const QString& text) { return std::filesystem::path(text.toStdWString()); }
QString F(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }
QString Storage(std::uint32_t flags) {
    switch (flags) { case 0: return "普通"; case 1: return "zlib 压缩"; case 2: return "完整加密";
                     case 3: return "压缩 + 完整加密"; case 4: return "前 1024 字节加密"; }
    return "未知";
}
QString Change(const std::optional<WorkspaceEntry>& record) {
    if (!record) return "";
    switch (record->change) { case ChangeKind::Keep: return ""; case ChangeKind::Replace: return "替换";
                            case ChangeKind::Add: return "新增"; case ChangeKind::Delete: return "删除"; }
    return "";
}
bool ChooseOutput(QWidget* parent, QString& directory, bool& overwrite, const QString& title) {
    QDialog dialog(parent); dialog.setWindowTitle(title); dialog.resize(620, 170);
    auto* layout = new QVBoxLayout(&dialog);
    auto* line = new QLineEdit(directory);
    auto* browse = new QPushButton("选择目录…");
    auto* row = new QHBoxLayout; row->addWidget(line, 1); row->addWidget(browse); layout->addLayout(row);
    auto* check = new QCheckBox("替换已有输出文件（默认跳过已有解包文件；封包默认报错）"); layout->addWidget(check);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("开始"); buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto chosen = QFileDialog::getExistingDirectory(&dialog, "选择输出目录", line->text()); if (!chosen.isEmpty()) line->setText(chosen);
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { if (!line->text().trimmed().isEmpty()) dialog.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    directory = line->text(); overwrite = check->isChecked(); return true;
}
void CopyStaged(const ArchiveWorkspace& workspace, const WorkspaceEntry& entry, const std::filesystem::path& target,
                bool overwrite, const ArchiveJob& job) {
    const auto input = workspace.InputFile(entry);
    if (Detail::SamePath(target, input)) throw ArchiveError(ErrorCode::Conflict, "输出不能覆盖工作区文件");
    Detail::File source(input); Detail::AtomicFile transaction(target, overwrite); Detail::File output(transaction.temporary, true);
    std::array<std::uint8_t, 65536> buffer{};
    for (std::uint64_t done = 0; done < source.Size();) {
        job.Check(); auto chunk = std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.Size() - done)));
        source.Read(chunk); output.Write(chunk); done += chunk.size();
    }
    job.Check(); output.Flush(); output.Close(); transaction.Commit();
}
}

class EntryModel : public QAbstractTableModel {
public:
    explicit EntryModel(QObject* parent) : QAbstractTableModel(parent) {}
    std::vector<EntryRow> rows;
    std::vector<QString> paths;
    std::vector<QString> packageNames;
    std::vector<ManagedPackage>* packages = nullptr;
    void Replace(std::vector<EntryRow> values) {
        beginResetModel(); rows = std::move(values);
        paths.clear(); paths.reserve(rows.size());
        for (const auto& row : rows) paths.push_back(Q(row.entry.path));
        packageNames.clear(); packageNames.reserve(packages->size());
        for (const auto& package : *packages) packageNames.push_back(Q(package.archive->packageName));
        endResetModel();
    }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows.size()); }
    int columnCount(const QModelIndex& = {}) const override { return 6; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static const QStringList names{"内部路径", "来源包", "文件大小", "存储大小", "存储方式", "待提交"};
        return names.value(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= static_cast<int>(rows.size())) return {};
        const auto& row = rows[static_cast<std::size_t>(index.row())];
        if (role == Qt::UserRole && (index.column() == 2 || index.column() == 3))
            return QVariant::fromValue<qulonglong>(index.column() == 2 ? row.entry.size : row.entry.storedSize);
        if (role == Qt::TextAlignmentRole && (index.column() == 2 || index.column() == 3)) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::FontRole && row.staged && row.staged->change == ChangeKind::Delete) { QFont font; font.setStrikeOut(true); return font; }
        if (role == Qt::ForegroundRole && row.staged && row.staged->change != ChangeKind::Keep) return QColor(row.staged->change == ChangeKind::Delete ? "#b54343" : "#137d83");
        if (role == Qt::ToolTipRole) return Q(row.entry.path) + "\nID: " + QString::number(row.entry.id) + "\n密钥材料: " + Q(Hex(row.entry.cipherKey));
        if (role != Qt::DisplayRole) return {};
        switch (index.column()) {
        case 0: return paths[static_cast<std::size_t>(index.row())];
        case 1: return packageNames[row.package];
        case 2: return QLocale().toString(static_cast<qulonglong>(row.entry.size)) + " B";
        case 3: return row.staged && row.staged->change != ChangeKind::Keep ? QString("封包时计算") : QLocale().toString(static_cast<qulonglong>(row.entry.storedSize)) + " B";
        case 4: return Storage(row.entry.flags);
        case 5: return Change(row.staged);
        }
        return {};
    }
};
class EntryProxy : public QSortFilterProxyModel {
public:
    explicit EntryProxy(QObject* parent) : QSortFilterProxyModel(parent) { setDynamicSortFilter(true); }
    QString query, extension;
    std::vector<std::pair<std::size_t, std::string>> scopes;
    // Rebuild the mapping in one layout change. Incremental row removal makes
    // fragmented filters on 164k entries spend quadratic time updating views.
    void Refresh() { invalidate(); }
protected:
    bool filterAcceptsRow(int number, const QModelIndex&) const override {
        const auto* model = static_cast<const EntryModel*>(sourceModel());
        const auto& row = model->rows[static_cast<std::size_t>(number)];
        const auto& path = model->paths[static_cast<std::size_t>(number)];
        if (!query.isEmpty() && !path.contains(query, Qt::CaseInsensitive)) return false;
        if (!extension.isEmpty() && !path.endsWith(extension, Qt::CaseInsensitive)) return false;
        if (scopes.empty()) return true;
        for (const auto& scope : scopes) if (scope.first == row.package && (scope.second.empty() || row.entry.lookupPath.starts_with(scope.second + "/"))) return true;
        return false;
    }
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const auto* model = static_cast<const EntryModel*>(sourceModel());
        const auto l = static_cast<std::size_t>(left.row()), r = static_cast<std::size_t>(right.row());
        const auto& a = model->rows[l]; const auto& b = model->rows[r];
        switch (left.column()) {
        case 0: return QString::compare(model->paths[l], model->paths[r], Qt::CaseInsensitive) < 0;
        case 1: return QString::compare(model->packageNames[a.package], model->packageNames[b.package], Qt::CaseInsensitive) < 0;
        case 2: return a.entry.size < b.entry.size;
        case 3: return a.entry.storedSize < b.entry.storedSize;
        default: return QString::compare(left.data().toString(), right.data().toString(), Qt::CaseInsensitive) < 0;
        }
    }
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PKNPacker · BubbleFighter 资源管理器"); resize(1380, 900); setMinimumSize(1040, 700);
    auto* shell = new QWidget; Ui::ManagerShell ui; ui.setupUi(shell); setCentralWidget(shell);
    auto* title = new QLabel("PKNPacker"); title->setObjectName("Title");
    auto* subtitle = new QLabel("准备修改  →  检查变更  →  生成新包  →  验证结果"); subtitle->setObjectName("Subtitle");
    auto* heading = new QHBoxLayout; heading->addWidget(title); heading->addSpacing(18); heading->addWidget(subtitle); heading->addStretch(); ui.rootLayout->addLayout(heading);
    auto* files = addToolBar("资源包"); files->setMovable(false);
    const auto action = [&](QToolBar* bar, const QString& name, std::function<void()> fn, bool mutation = true) {
        auto* a = bar->addAction(name); connect(a, &QAction::triggered, this, [fn] { fn(); }); if (mutation) mutationActions_.push_back(a); return a;
    };
    action(files, "打开 PKN…", [this] { OpenPaths(QFileDialog::getOpenFileNames(this, "打开资源包", {}, "PKN 资源包 (*.pkn)")); });
    action(files, "扫描目录…", [this] { const auto directory = QFileDialog::getExistingDirectory(this, "扫描 Packs 目录"); if (!directory.isEmpty()) OpenPaths({directory}); });
    action(files, "打开工作区…", [this] { const auto directory = QFileDialog::getExistingDirectory(this, "打开包含 WorkspaceManifest.json 的目录"); if (!directory.isEmpty()) OpenWorkspaces({directory}); });
    action(files, "新建 PKN…", [this] { CreatePackage(); }); files->addSeparator();
    action(files, "完整解包…", [this] { ExtractPackages(); });
    action(files, "选择解包…", [this] { ExtractPackages(true); });
    action(files, "批量封包…", [this] { PackWorkspaces(); });
    action(files, "验证资源包", [this] { VerifyPackages(); });
    auto* edits = new QToolBar("文件操作"); edits->setMovable(false); ui.rootLayout->addWidget(edits);
    action(edits, "拆包", [this] { ExportSelected(true); })->setToolTip("将选中的单个文件完整拆包到指定位置");
    action(edits, "导出选中文件…", [this] { ExportSelected(false); });
    edits->addSeparator();
    action(edits, "替换…", [this] { StageReplace(); }); action(edits, "新增…", [this] { StageAdd(); });
    action(edits, "删除", [this] { StageDelete(false); }); action(edits, "撤销操作", [this] { StageDelete(true); });
    action(edits, "检查变更", [this] { CheckChanges(); });
    auto* filters = new QHBoxLayout;
    search_ = new QLineEdit; search_->setObjectName("PathSearch"); search_->setPlaceholderText("搜索内部路径   Ctrl+F"); filters->addWidget(search_, 1);
    extensions_ = new QComboBox; extensions_->setObjectName("ExtensionFilter"); extensions_->addItem("所有扩展名", ""); filters->addWidget(extensions_);
    scopeMode_ = new QComboBox; scopeMode_->addItems({"浏览树中选定目录", "浏览全部已打开包"}); filters->addWidget(scopeMode_); ui.rootLayout->addLayout(filters);
    auto* find = new QAction(this); find->setShortcut(QKeySequence::Find); addAction(find); connect(find, &QAction::triggered, search_, [this] { search_->setFocus(); search_->selectAll(); });
    auto* splitter = new QSplitter(Qt::Horizontal);
    tree_ = new QTreeWidget; tree_->setHeaderLabel("资源包与目录"); tree_->setSelectionMode(QAbstractItemView::ExtendedSelection); tree_->setMinimumWidth(210); splitter->addWidget(tree_);
    auto* right = new QSplitter(Qt::Vertical);
    table_ = new QTableView; table_->setObjectName("EntryTable"); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setAlternatingRowColors(true); table_->setSortingEnabled(true); table_->verticalHeader()->hide();
    model_ = new EntryModel(this); model_->packages = &packages_; proxy_ = new EntryProxy(this); proxy_->setSourceModel(model_); table_->setModel(proxy_);
    table_->horizontalHeader()->setStretchLastSection(true); table_->setColumnWidth(0, 420); table_->setColumnWidth(1, 190); table_->setColumnWidth(2, 110); table_->setColumnWidth(3, 110); table_->setColumnWidth(4, 155);
    right->addWidget(table_);
    auto* preview = new QWidget; auto* previewLayout = new QVBoxLayout(preview); previewLayout->setContentsMargins(0, 0, 0, 0);
    previewInfo_ = new QLabel("选择文件查看文本或十六进制预览"); previewInfo_->setWordWrap(true);
    auto* previewHeading = new QHBoxLayout; previewHeading->addWidget(previewInfo_, 1); previousPage_ = new QPushButton("上一页"); nextPage_ = new QPushButton("下一页");
    previousPage_->setEnabled(false); nextPage_->setEnabled(false); previewHeading->addWidget(previousPage_); previewHeading->addWidget(nextPage_); previewLayout->addLayout(previewHeading);
    auto* tabs = new QTabWidget;
    textPreview_ = new QPlainTextEdit; hexPreview_ = new QPlainTextEdit;
    for (auto* edit : {textPreview_, hexPreview_}) { edit->setReadOnly(true); edit->setLineWrapMode(QPlainTextEdit::NoWrap); edit->setFont(QFont("Consolas", 10)); }
    tabs->addTab(textPreview_, "文本（首次最多 1 MiB）"); tabs->addTab(hexPreview_, "十六进制（每页 4 KiB）"); previewLayout->addWidget(tabs); right->addWidget(preview);
    right->setSizes({470, 220}); splitter->addWidget(right); splitter->setStretchFactor(1, 1); splitter->setSizes({270, 1070}); ui.rootLayout->addWidget(splitter, 1);
    information_ = new QLabel("尚未打开资源包"); ui.rootLayout->addWidget(information_);
    auto* taskHeading = new QHBoxLayout;
    taskInfo_ = new QLabel("任务就绪"); taskHeading->addWidget(taskInfo_, 1);
    cancel_ = new QPushButton("取消当前任务"); cancel_->setEnabled(false); taskHeading->addWidget(cancel_);
    retry_ = new QPushButton("重试失败任务"); retry_->setEnabled(false); taskHeading->addWidget(retry_); ui.rootLayout->addLayout(taskHeading);
    auto* progressRow = new QHBoxLayout; progressRow->addWidget(new QLabel("总体")); overall_ = new QProgressBar; overall_->setRange(0, 1000); progressRow->addWidget(overall_);
    progressRow->addWidget(new QLabel("当前")); current_ = new QProgressBar; current_->setRange(0, 1000); progressRow->addWidget(current_); ui.rootLayout->addLayout(progressRow);
    results_ = new QTableWidget(0, 3); results_->setHorizontalHeaderLabels({"任务 / 文件", "结果", "说明"}); results_->setEditTriggers(QAbstractItemView::NoEditTriggers); results_->verticalHeader()->hide();
    results_->horizontalHeader()->setStretchLastSection(true); results_->setColumnWidth(0, 350); results_->setColumnWidth(1, 90); results_->setMaximumHeight(125); ui.rootLayout->addWidget(results_);
    statusBar()->showMessage("PKN v2 · 包名决定头部与索引密钥 · 封包后自动完整验证");
    previewPool_.setMaxThreadCount(1);
    auto* searchTimer = new QTimer(this); searchTimer->setSingleShot(true); searchTimer->setInterval(200);
    connect(search_, &QLineEdit::textChanged, this, [this, searchTimer] {
        if (previewCancelled_) previewCancelled_->store(true); ++previewGeneration_;
        searchTimer->start();
    });
    connect(searchTimer, &QTimer::timeout, this, [this] {
        proxy_->query = search_->text(); proxy_->Refresh();
        information_->setText(QString("%1 个包 · %2 个文件 · 筛选后 %3 个").arg(packages_.size()).arg(model_->rows.size()).arg(proxy_->rowCount()));
    });
    connect(extensions_, &QComboBox::currentIndexChanged, this, [this] { proxy_->extension = extensions_->currentData().toString(); proxy_->Refresh(); });
    connect(scopeMode_, &QComboBox::currentIndexChanged, this, [this] { SetScopes(); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] { SetScopes(); });
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { Preview(); });
    connect(previousPage_, &QPushButton::clicked, this, [this] { Preview(previewOffset_ >= 4096 ? previewOffset_ - 4096 : 0); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { Preview(previewOffset_ + 4096); });
    connect(cancel_, &QPushButton::clicked, this, [this] { job_.cancelled->store(true); cancel_->setEnabled(false); taskInfo_->setText("正在取消，保留已完成结果…"); });
    connect(retry_, &QPushButton::clicked, this, [this] { auto tasks = failedTasks_; auto complete = failedComplete_; RunTasks("重试失败任务", std::move(tasks), std::move(complete)); });
    connect(table_, &QTableView::doubleClicked, this, [this] { Preview(); });
    results_->setToolTip("双击解包／导出任务查看逐文件结果");
    connect(results_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || static_cast<std::size_t>(row) >= finishedTasks_.size() || !finishedTasks_[row].details) return;
        QDialog dialog(this); dialog.setWindowTitle("逐文件结果 · " + finishedTasks_[row].label); dialog.resize(900, 580);
        auto* layout = new QVBoxLayout(&dialog); auto* files = new QTreeWidget; files->setHeaderLabels({"内部路径", "结果", "说明"}); layout->addWidget(files);
        for (const auto& item : finishedTasks_[row].details->items) new QTreeWidgetItem(files, {Q(item.item),
            item.status == ItemStatus::Success ? "成功" : item.status == ItemStatus::Skipped ? "跳过" : item.status == ItemStatus::Cancelled ? "取消" : "失败", Q(item.message)});
        files->setColumnWidth(0, 550); files->setColumnWidth(1, 80);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); dialog.exec();
    });
    const auto geometry = QSettings().value("window/geometry").toByteArray(); if (!geometry.isEmpty()) restoreGeometry(geometry);
}
MainWindow::~MainWindow() {
    job_.cancelled->store(true); if (previewCancelled_) previewCancelled_->store(true);
    if (worker_.joinable()) worker_.join(); previewPool_.waitForDone();
    QSettings().setValue("window/geometry", saveGeometry());
}
void MainWindow::ShowError(const QString& message) { QMessageBox::warning(this, "PKNPacker", message); }
void MainWindow::RunTasks(const QString& title, std::vector<GuiTask> tasks, std::function<void(const JobResult&)> complete) {
    if (busy_) { statusBar()->showMessage("已有任务正在执行，完成或取消后可开始新任务", 5000); return; }
    if (tasks.empty()) return;
    busy_ = true; job_ = ArchiveJob{}; cancel_->setEnabled(true); retry_->setEnabled(false); failedTasks_.clear();
    finishedTasks_.clear(); for (const auto& task : tasks) if (task.details) task.details->items.clear();
    for (auto* action : mutationActions_) action->setEnabled(false);
    taskInfo_->setText(title + " · 默认并行处理两个独立资源包"); overall_->setValue(0); current_->setValue(0); results_->setRowCount(0);
    const auto token = job_.cancelled;
    worker_ = std::jthread([this, title, tasks = std::move(tasks), complete = std::move(complete), token] {
        ArchiveJob job; job.cancelled = token;
        std::atomic_size_t completed = 0;
        std::mutex throttle;
        auto previous = std::chrono::steady_clock::time_point{};
        job.progress = [&, this](const JobProgress& value) {
            std::lock_guard lock(throttle);
            const auto now = std::chrono::steady_clock::now();
            if (value.completed && now - previous < std::chrono::milliseconds(120)) return;
            previous = now;
            const auto totalValue = static_cast<int>(completed.load() * 1000 / tasks.size());
            const auto currentValue = value.total ? static_cast<int>(std::min<std::uint64_t>(1000, value.completed * 1000 / value.total)) : 0;
            QMetaObject::invokeMethod(this, [this, value, totalValue, currentValue] {
                overall_->setValue(totalValue); current_->setValue(currentValue);
                taskInfo_->setText(Q(value.stage) + " · " + Q(value.item));
            }, Qt::QueuedConnection);
        };
        std::vector<std::pair<std::string, std::function<void()>>> operations;
        for (const auto& task : tasks) operations.emplace_back(U(task.label), [&, task] { task.action(job); completed.fetch_add(1); });
        const auto result = RunArchiveJobs(operations, job, 2);
        QMetaObject::invokeMethod(this, [this, title, tasks, result, complete, token] {
            busy_ = false; cancel_->setEnabled(false); for (auto* action : mutationActions_) action->setEnabled(true);
            failedTasks_.clear();
            finishedTasks_ = tasks;
            for (std::size_t i = 0; i < result.items.size(); ++i) {
                const auto& item = result.items[i];
                const int row = results_->rowCount(); results_->insertRow(row);
                results_->setItem(row, 0, new QTableWidgetItem(Q(item.item)));
                results_->setItem(row, 1, new QTableWidgetItem(item.status == ItemStatus::Success ? "成功" : item.status == ItemStatus::Cancelled ? "取消" : item.status == ItemStatus::Skipped ? "跳过" : "失败"));
                auto message = Q(item.message);
                if (tasks[i].details) {
                    unsigned success = 0, skipped = 0, failed = 0, cancelled = 0;
                    for (const auto& file : tasks[i].details->items) {
                        if (file.status == ItemStatus::Success) ++success;
                        else if (file.status == ItemStatus::Skipped) ++skipped;
                        else if (file.status == ItemStatus::Cancelled) ++cancelled;
                        else ++failed;
                    }
                    message = QString("成功 %1 · 跳过 %2 · 失败 %3 · 取消 %4；双击查看明细").arg(success).arg(skipped).arg(failed).arg(cancelled) + (message.isEmpty() ? "" : "；" + message);
                    if (item.status == ItemStatus::Success && skipped && !success) results_->item(row, 1)->setText("跳过");
                }
                results_->setItem(row, 2, new QTableWidgetItem(message));
                if (item.status == ItemStatus::Failed || item.status == ItemStatus::Cancelled) failedTasks_.push_back(tasks[i]);
            }
            retry_->setEnabled(!failedTasks_.empty());
            failedComplete_ = failedTasks_.empty() ? std::function<void(const JobResult&)>{} : complete;
            if (!token->load()) overall_->setValue(1000);
            current_->setValue(result.Succeeded() ? 1000 : current_->value());
            taskInfo_->setText(title + (token->load() ? " · 已取消" : result.Succeeded() ? " · 完成" : " · 部分任务失败"));
            if (complete) complete(result);
        }, Qt::QueuedConnection);
    });
}
void MainWindow::OpenPaths(const QStringList& inputs) {
    QStringList paths;
    for (const auto& input : inputs) {
        if (QFileInfo(input).isDir()) {
            QDir directory(input); for (const auto& name : directory.entryList({"*.pkn", "*.PKN"}, QDir::Files)) paths.append(directory.absoluteFilePath(name));
        } else paths.append(QFileInfo(input).absoluteFilePath());
    }
    paths.removeDuplicates();
    auto opened = std::make_shared<std::vector<std::shared_ptr<ArchiveCatalog>>>(); auto mutex = std::make_shared<std::mutex>();
    std::vector<GuiTask> tasks;
    for (const auto& path : paths) tasks.push_back({QFileInfo(path).fileName(), [opened, mutex, path](const ArchiveJob& job) {
        auto archive = std::make_shared<ArchiveCatalog>(OpenArchive(P(path), {}, job)); std::lock_guard lock(*mutex); opened->push_back(std::move(archive));
    }});
    RunTasks("打开资源包", std::move(tasks), [this, opened](const JobResult&) {
        for (const auto& archive : *opened) {
            const auto duplicate = std::any_of(packages_.begin(), packages_.end(), [&](const auto& package) { return package.archive->source == archive->source; });
            if (!duplicate) packages_.push_back({archive, {}});
        }
        RebuildCatalog(); if (!smokeReport_.isEmpty() || !smokeScreenshot_.isEmpty()) QTimer::singleShot(200, this, [this] { RunSmokeTest(); });
    });
}
void MainWindow::OpenWorkspaces(const QStringList& paths) {
    auto opened = std::make_shared<std::vector<ManagedPackage>>(); auto mutex = std::make_shared<std::mutex>(); std::vector<GuiTask> tasks;
    for (const auto& path : paths) tasks.push_back({path, [opened, mutex, path](const ArchiveJob& job) {
        auto workspace = std::make_shared<ArchiveWorkspace>(ArchiveWorkspace::Load(P(path)));
        auto archive = std::make_shared<ArchiveCatalog>();
        if (!workspace->manifest.source.empty()) *archive = OpenArchive(workspace->manifest.source, {workspace->manifest.logicalDirectory, workspace->manifest.originalPackageName}, job);
        else archive->packageName = workspace->manifest.packageName;
        std::lock_guard lock(*mutex); opened->push_back({archive, workspace});
    }});
    RunTasks("打开工作区", std::move(tasks), [this, opened](const JobResult&) {
        for (const auto& package : *opened) {
            const auto found = std::find_if(packages_.begin(), packages_.end(), [&](const auto& existing) {
                return !package.archive->source.empty() && existing.archive->source == package.archive->source;
            });
            if (found != packages_.end()) found->workspace = package.workspace; else packages_.push_back(package);
        }
        RebuildCatalog();
    });
}
void MainWindow::RebuildCatalog() {
    RecordSmokeStage("catalog");
    tree_->blockSignals(true); tree_->clear();
    auto* all = new QTreeWidgetItem(tree_, {"全部资源包"}); all->setData(0, Qt::UserRole, -1); all->setExpanded(true);
    std::vector<EntryRow> rows; std::set<QString> extensions;
    for (std::size_t i = 0; i < packages_.size(); ++i) {
        const auto& package = packages_[i]; auto* node = new QTreeWidgetItem(all, {Q(package.archive->packageName) + (package.workspace ? "  [工作区]" : "")});
        node->setData(0, Qt::UserRole, static_cast<qulonglong>(i)); node->setData(0, Qt::UserRole + 1, ""); node->setToolTip(0, package.workspace ? F(package.workspace->root) : F(package.archive->source));
        std::vector<EntryRow> packageRows;
        for (const auto& entry : package.archive->entries) packageRows.push_back({i, entry, {}});
        std::unordered_map<EntryId, std::size_t> entryPositions;
        for (std::size_t position = 0; position < packageRows.size(); ++position) entryPositions.emplace(packageRows[position].entry.id, position);
        if (package.workspace) for (const auto& record : package.workspace->manifest.entries) {
            const auto existing = entryPositions.find(record.id);
            std::size_t position;
            if (existing == entryPositions.end()) {
                ArchiveEntry entry; entry.id = record.id; entry.path = record.path; entry.rawPath = FromUtf8(record.path); entry.lookupPath = NormalizePath(record.path); entry.flags = record.flags;
                position = packageRows.size(); packageRows.push_back({i, entry, record}); entryPositions.emplace(record.id, position);
            } else { position = existing->second; packageRows[position].staged = record; }
            auto& row = packageRows[position];
            if (record.change == ChangeKind::Add || record.change == ChangeKind::Replace) {
                std::error_code error; const auto size = std::filesystem::file_size(package.workspace->InputFile(record), error); if (!error) row.entry.size = size;
                row.entry.flags = record.flags;
            }
        }
        std::map<std::string, QTreeWidgetItem*> directories; directories[""] = node;
        for (const auto& row : packageRows) {
            std::size_t start = 0;
            while (true) {
                const auto slash = row.entry.lookupPath.find('/', start); if (slash == std::string::npos) break;
                const auto prefix = row.entry.lookupPath.substr(0, slash);
                if (!directories.contains(prefix)) {
                    const auto parentSlash = prefix.rfind('/'); const auto parentPrefix = parentSlash == std::string::npos ? "" : prefix.substr(0, parentSlash);
                    auto* child = new QTreeWidgetItem(directories.at(parentPrefix), {Q(prefix.substr(parentSlash == std::string::npos ? 0 : parentSlash + 1))});
                    child->setData(0, Qt::UserRole, static_cast<qulonglong>(i)); child->setData(0, Qt::UserRole + 1, Q(prefix)); directories[prefix] = child;
                }
                start = slash + 1;
            }
            const auto extension = QFileInfo(Q(row.entry.path)).suffix().toLower(); if (!extension.isEmpty()) extensions.insert("." + extension);
            rows.push_back(row);
        }
    }
    RecordSmokeStage("tree_ready");
    tree_->blockSignals(false); all->setSelected(true);
    model_->Replace(std::move(rows));
    RecordSmokeStage("model_ready");
    const auto previousExtension = extensions_->currentData().toString(); extensions_->blockSignals(true); extensions_->clear(); extensions_->addItem("所有扩展名", "");
    for (const auto& extension : extensions) extensions_->addItem(extension, extension);
    const auto extensionIndex = extensions_->findData(previousExtension); if (extensionIndex >= 0) extensions_->setCurrentIndex(extensionIndex); extensions_->blockSignals(false);
    SetScopes(); table_->sortByColumn(0, Qt::AscendingOrder);
    information_->setText(QString("%1 个包 · %2 个文件 · 筛选后 %3 个").arg(packages_.size()).arg(model_->rows.size()).arg(proxy_->rowCount()));
    RecordSmokeStage("catalog_ready");
}
void MainWindow::SetScopes() {
    proxy_->scopes.clear();
    if (scopeMode_->currentIndex() == 0) for (const auto* item : tree_->selectedItems()) {
        if (item->data(0, Qt::UserRole).toLongLong() < 0) { proxy_->scopes.clear(); break; }
        proxy_->scopes.emplace_back(static_cast<std::size_t>(item->data(0, Qt::UserRole).toULongLong()), U(item->data(0, Qt::UserRole + 1).toString()));
    }
    proxy_->Refresh();
}
std::vector<EntryRow> MainWindow::SelectedRows() const {
    std::vector<EntryRow> selected;
    for (const auto& index : table_->selectionModel()->selectedRows()) selected.push_back(model_->rows[static_cast<std::size_t>(proxy_->mapToSource(index).row())]);
    return selected;
}
std::vector<std::size_t> MainWindow::SelectedPackages() const {
    std::set<std::size_t> indexes;
    for (const auto& row : SelectedRows()) indexes.insert(row.package);
    if (indexes.empty()) for (const auto* item : tree_->selectedItems()) {
        if (item->data(0, Qt::UserRole).toLongLong() < 0) { for (std::size_t i = 0; i < packages_.size(); ++i) indexes.insert(i); break; }
        indexes.insert(static_cast<std::size_t>(item->data(0, Qt::UserRole).toULongLong()));
    }
    return {indexes.begin(), indexes.end()};
}
void MainWindow::Preview(std::uint64_t offset) {
    if (previewCancelled_) previewCancelled_->store(true);
    const auto generation = ++previewGeneration_;
    const auto selected = SelectedRows();
    if (selected.size() != 1) { previewInfo_->setText(selected.empty() ? "选择文件查看预览" : "已多选，预览仅显示单个文件"); return; }
    const auto row = selected.front(); const auto package = packages_[row.package];
    if (offset >= row.entry.size && row.entry.size) return;
    previewCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto token = previewCancelled_;
    previewOffset_ = offset;
    previewInfo_->setText(Q(row.entry.path) + " · 正在读取预览…");
    previewPool_.start([this, package, row, offset, generation, token] {
        try {
            ArchiveJob job; job.cancelled = token;
            std::vector<std::uint8_t> bytes;
            const auto limit = offset ? 4096ULL : 1024ULL * 1024;
            if (row.staged && row.staged->extracted && row.staged->change != ChangeKind::Delete && package.workspace) {
                Detail::File file(package.workspace->InputFile(*row.staged));
                if (offset > file.Size()) throw ArchiveError(ErrorCode::Bounds, "预览偏移超出文件");
                file.Seek(offset); bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(limit, file.Size() - offset))); file.Read(bytes);
            } else bytes = ReadEntry(*package.archive, row.entry.id, limit, offset, job);
            job.Check();
            QString hex;
            for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 4096); i += 16) {
                hex += QString::number(offset + i, 16).rightJustified(8, '0') + "  "; QString ascii;
                for (std::size_t j = 0; j < 16; ++j) {
                    if (i + j < bytes.size()) { const auto value = bytes[i + j]; hex += QString::number(value, 16).rightJustified(2, '0') + " "; ascii += value >= 32 && value < 127 ? QChar(value) : QChar('.'); }
                    else hex += "   ";
                    if (j == 7) hex += ' ';
                }
                hex += " " + ascii + "\n";
            }
            QString text, encoding;
            if (!offset) {
                const auto view = QByteArrayView(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
                QStringConverter::Encoding codec = QStringConverter::Utf8;
                if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) { codec = QStringConverter::Utf16LE; encoding = "UTF-16 LE"; }
                else if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) { codec = QStringConverter::Utf16BE; encoding = "UTF-16 BE"; }
                else encoding = "UTF-8";
                QStringDecoder decoder(codec); text = decoder(view);
                bool controls = false;
                for (const auto c : text) if (c.unicode() < 32 && c != '\r' && c != '\n' && c != '\t') { controls = true; break; }
                if (decoder.hasError() || controls) { text = "未能可靠识别文本编码，请查看十六进制页。"; encoding = "二进制 / 未识别文本"; }
                else if (text.isEmpty()) text = "（空文件）";
            }
            QMetaObject::invokeMethod(this, [this, row, offset, generation, text, hex, encoding] {
                if (generation != previewGeneration_) return;
                hexPreview_->setPlainText(hex); if (!offset) textPreview_->setPlainText(text);
                previewInfo_->setText(Q(row.entry.path) + " · " + (offset ? "十六进制" : encoding) + " · 偏移 " + QString::number(offset)
                     + (row.entry.size > 1024 * 1024 && !offset ? " · 文本已截取，导出读取完整内容" : ""));
                previousPage_->setEnabled(offset != 0); nextPage_->setEnabled(offset + 4096 < row.entry.size);
            }, Qt::QueuedConnection);
        } catch (const ArchiveError& error) {
            if (error.Code() == ErrorCode::Cancelled) return;
            QMetaObject::invokeMethod(this, [this, generation, message = Q(error.what())] { if (generation == previewGeneration_) previewInfo_->setText(message); }, Qt::QueuedConnection);
        } catch (const std::exception& error) {
            QMetaObject::invokeMethod(this, [this, generation, message = Q(error.what())] { if (generation == previewGeneration_) previewInfo_->setText(message); }, Qt::QueuedConnection);
        }
    });
}
void MainWindow::ExportSelected(bool singleFile) {
    auto rows = SelectedRows();
    if (rows.empty() && !singleFile) for (int i = 0; i < proxy_->rowCount(); ++i)
        rows.push_back(model_->rows[static_cast<std::size_t>(proxy_->mapToSource(proxy_->index(i, 0)).row())]);
    if (rows.empty()) { ShowError(singleFile ? "请先在文件列表中选中一个文件，再点击拆包。" : "请在文件列表中选择文件，或在目录树中选择一个目录。"); return; }
    if (singleFile && rows.size() != 1) { ShowError("拆包需要选中一个文件；批量导出请使用导出选中文件。"); return; }
    QString destination; bool overwrite = false;
    if (singleFile) {
        destination = QFileDialog::getSaveFileName(this, "拆包", QFileInfo(Q(rows.front().entry.path)).fileName()); if (destination.isEmpty()) return;
        overwrite = QFileInfo::exists(destination); // QFileDialog already obtained explicit replacement consent.
    } else if (!ChooseOutput(this, destination, overwrite, "导出选中文件（按来源包保留目录）")) return;
    std::vector<GuiTask> tasks;
    // One task per package: two packages in parallel, writes within a package are serial.
    std::map<std::size_t, std::vector<EntryRow>> groups; for (const auto& row : rows) groups[row.package].push_back(row);
    std::set<std::string> outputNames;
    for (const auto& [index, entries] : groups) {
        const auto package = packages_[index];
        if (!singleFile && !outputNames.insert(NormalizePath(package.archive->packageName)).second) { ShowError("不同输入包会导出到同名目录，请分开处理。"); return; }
        auto details = std::make_shared<JobResult>();
        tasks.push_back({Q(package.archive->packageName), [package, entries, destination, overwrite, singleFile, details](const ArchiveJob& job) {
            std::uint64_t done = 0; Json outcomes = Json::array(); bool failed = false;
            for (const auto& row : entries) {
                job.Report(singleFile ? "拆包" : "导出", row.entry.path, done++, entries.size());
                const auto target = singleFile ? P(destination) : Detail::SafeChild(P(destination) / Detail::Utf8Path(package.archive->packageName).stem() / "Files", row.entry.path);
                if (std::filesystem::exists(target) && !overwrite) { outcomes.push_back({{"path", row.entry.path}, {"status", "skipped"}}); details->items.push_back({row.entry.path, ItemStatus::Skipped, "目标已存在"}); continue; }
                try {
                    if (row.staged && row.staged->change == ChangeKind::Delete) throw ArchiveError(ErrorCode::Conflict, "文件已登记删除，请撤销后导出");
                    if (row.staged && row.staged->extracted && package.workspace) CopyStaged(*package.workspace, *row.staged, target, overwrite, job);
                    else ExportEntry(*package.archive, row.entry.id, target, overwrite, job);
                    outcomes.push_back({{"path", row.entry.path}, {"status", "success"}});
                    details->items.push_back({row.entry.path, ItemStatus::Success, ""});
                } catch (const ArchiveError& error) {
                    details->items.push_back({row.entry.path, error.Code() == ErrorCode::Cancelled ? ItemStatus::Cancelled : ItemStatus::Failed, error.what()});
                    if (error.Code() == ErrorCode::Cancelled) throw;
                    failed = true; outcomes.push_back({{"path", row.entry.path}, {"status", "failed"}, {"message", error.what()}});
                }
            }
            if (!singleFile) Detail::WriteTextAtomic(P(destination) / Detail::Utf8Path(package.archive->packageName).stem() / "ExportReport.json", outcomes.dump(2) + "\n");
            if (failed) throw ArchiveError(ErrorCode::Io, singleFile ? "拆包失败，请查看任务详情。" : "部分文件导出失败，详见 ExportReport.json");
        }, details});
    }
    RunTasks(singleFile ? "拆包" : "导出文件", std::move(tasks));
}
void MainWindow::ExtractPackages(bool selectionOnly, std::function<void(const JobResult&)> complete) {
    std::map<std::size_t, std::vector<EntryId>> selected;
    if (selectionOnly) {
        auto rows = SelectedRows();
        if (rows.empty()) for (int i = 0; i < proxy_->rowCount(); ++i)
            rows.push_back(model_->rows[static_cast<std::size_t>(proxy_->mapToSource(proxy_->index(i, 0)).row())]);
        for (const auto& row : rows) {
            if (packages_[row.package].archive->source.empty() || (row.staged && row.staged->change != ChangeKind::Keep)) {
                ShowError("选择解包读取原包；当前选择含待提交变更，请生成新包后解包，或使用导出选中文件保存变更内容。"); return;
            }
            selected[row.package].push_back(row.entry.id);
        }
    } else for (const auto index : SelectedPackages()) {
        const auto archive = packages_[index].archive;
        if (archive->source.empty()) { ShowError("当前包尚未生成，请先检查变更并封包。"); return; }
        for (const auto& entry : archive->entries) selected[index].push_back(entry.id);
    }
    if (selected.empty()) { ShowError("请先打开资源包并选择文件或目录。"); return; }
    QString destination; bool overwrite = false;
    if (!ChooseOutput(this, destination, overwrite, selectionOnly ? "选择解包原包与重封清单" : "完整解包与重封清单")) return;
    std::vector<GuiTask> tasks;
    std::set<std::string> outputNames;
    for (const auto& [index, ids] : selected) {
        const auto archive = packages_[index].archive;
        if (!outputNames.insert(NormalizePath(archive->packageName)).second) { ShowError("不同输入包会解包到同名目录，请分开处理。"); return; }
        auto details = std::make_shared<JobResult>();
        tasks.push_back({Q(archive->packageName), [archive, ids, destination, overwrite, details](const ArchiveJob& job) {
            const auto root = P(destination) / Detail::Utf8Path(archive->packageName).stem();
            *details = ExtractEntries(*archive, ids, root, {overwrite, true}, job);
            const auto& result = *details;
            Json items = Json::array();
            for (const auto& item : result.items) items.push_back({{"path", item.item}, {"status", item.status == ItemStatus::Success ? "success" : item.status == ItemStatus::Skipped ? "skipped" : item.status == ItemStatus::Cancelled ? "cancelled" : "failed"}, {"message", item.message}});
            Detail::WriteTextAtomic(root / "ExtractionReport.json", items.dump(2) + "\n");
            if (!result.Succeeded()) throw ArchiveError(job.cancelled->load() ? ErrorCode::Cancelled : ErrorCode::Io, "部分文件未完成，详见 ExtractionReport.json");
        }, details});
    }
    RunTasks(selectionOnly ? "选择解包原包" : "完整解包", std::move(tasks), std::move(complete));
}
std::filesystem::path MainWindow::DefaultWorkspaceRoot() const {
    auto current = P(QCoreApplication::applicationDirPath());
    while (!current.empty() && current != current.parent_path()) {
        if (std::filesystem::exists(current / "AGENTS.md") && current.filename() == "PKNPacker") return current / "Artifacts/Workspaces";
        current = current.parent_path();
    }
    return P(QCoreApplication::applicationDirPath()) / "Workspaces";
}
void MainWindow::EnsureWorkspace(std::size_t index, std::function<void(std::shared_ptr<ArchiveWorkspace>)> complete) {
    if (packages_[index].workspace) { complete(packages_[index].workspace); return; }
    const auto archive = packages_[index].archive;
    const auto root = DefaultWorkspaceRoot() / Detail::Utf8Path(archive->packageName).stem();
    auto holder = std::make_shared<std::shared_ptr<ArchiveWorkspace>>();
    RunTasks("准备编辑工作区", {{Q(archive->packageName), [archive, root, holder](const ArchiveJob& job) {
        if (std::filesystem::exists(root / "WorkspaceManifest.json")) {
            auto workspace = ArchiveWorkspace::Load(root);
            if (workspace.manifest.source != archive->source || workspace.manifest.sourceSha256 != HashFile(archive->source, job).sha256)
                throw ArchiveError(ErrorCode::Conflict, "默认工作区属于其他原包，请打开独立工作区");
            *holder = std::make_shared<ArchiveWorkspace>(std::move(workspace));
        } else *holder = std::make_shared<ArchiveWorkspace>(ArchiveWorkspace::FromArchive(*archive, root, job));
    }}}, [this, index, holder, complete](const JobResult& result) {
        if (result.Succeeded() && *holder) { packages_[index].workspace = *holder; RebuildCatalog(); complete(*holder); }
    });
}
void MainWindow::StageReplace() {
    const auto rows = SelectedRows(); if (rows.size() != 1) { ShowError("替换时请选择一个文件。"); return; }
    const auto input = QFileDialog::getOpenFileName(this, "选择替换内容（资源内容由外部编辑）"); if (input.isEmpty()) return;
    const auto row = rows.front(); EnsureWorkspace(row.package, [this, row, input](auto workspace) {
        RunTasks("准备替换", {{Q(row.entry.path), [workspace, row, input](const ArchiveJob& job) { workspace->Replace(row.entry.id, P(input), job); }}}, [this](const JobResult&) { RebuildCatalog(); });
    });
}
void MainWindow::StageAdd() {
    const auto indexes = SelectedPackages(); if (indexes.size() != 1) { ShowError("新增时请选择一个目标资源包。"); return; }
    const auto input = QFileDialog::getOpenFileName(this, "选择新增文件"); if (input.isEmpty()) return;
    bool ok = false; const auto path = QInputDialog::getText(this, "新增资源", "完整内部路径（例如 Images/Custom/example.png）", QLineEdit::Normal, QFileInfo(input).fileName(), &ok);
    if (!ok || path.isEmpty()) return;
    try { NormalizePath(U(path)); } catch (const std::exception& error) { ShowError(Q(error.what())); return; }
    EnsureWorkspace(indexes.front(), [this, input, path](auto workspace) {
        RunTasks("准备新增", {{path, [workspace, input, path](const ArchiveJob& job) { workspace->Add(U(path), P(input), 3, job); }}}, [this](const JobResult&) { RebuildCatalog(); });
    });
}
void MainWindow::StageDelete(bool undo) {
    const auto rows = SelectedRows(); if (rows.empty()) { ShowError("请选择要处理的文件。"); return; }
    const auto index = rows.front().package;
    if (std::any_of(rows.begin(), rows.end(), [=](const auto& row) { return row.package != index; })) { ShowError("一次编辑一个包；批量封包支持多个独立包。"); return; }
    EnsureWorkspace(index, [this, rows, undo](auto workspace) {
        RunTasks(undo ? "撤销待提交操作" : "登记删除", {{Q(workspace->manifest.packageName), [workspace, rows, undo](const ArchiveJob& job) {
            for (const auto& row : rows) { job.Check(); if (undo) workspace->Undo(row.entry.id); else workspace->Delete(row.entry.id); }
        }}}, [this](const JobResult&) { RebuildCatalog(); });
    });
}
void MainWindow::ShowChanges(const std::vector<std::pair<QString, ChangeSummary>>& changes, std::function<void()> build) {
    QDialog dialog(this); dialog.setWindowTitle("检查变更"); dialog.resize(830, 560); auto* layout = new QVBoxLayout(&dialog);
    auto* tree = new QTreeWidget; tree->setHeaderLabels({"资源包 / 操作", "路径或问题"}); layout->addWidget(tree);
    bool valid = true;
    for (const auto& [name, change] : changes) {
        auto* node = new QTreeWidgetItem(tree, {name, QString("新增 %1 · 替换 %2 · 删除 %3 · 冲突 %4").arg(change.added.size()).arg(change.replaced.size()).arg(change.deleted.size()).arg(change.conflicts.size())}); node->setExpanded(true);
        for (const auto& path : change.added) new QTreeWidgetItem(node, {"新增", Q(path)});
        for (const auto& path : change.replaced) new QTreeWidgetItem(node, {"替换", Q(path)});
        for (const auto& path : change.deleted) new QTreeWidgetItem(node, {"删除", Q(path)});
        for (const auto& path : change.conflicts) new QTreeWidgetItem(node, {"冲突", Q(path)});
        valid = valid && change.Valid();
    }
    tree->setColumnWidth(0, 230);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(buttons);
    if (build) { auto* generate = buttons->addButton("生成新包…", QDialogButtonBox::AcceptRole); generate->setEnabled(valid); connect(generate, &QPushButton::clicked, &dialog, &QDialog::accept); }
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted && build) build();
}
void MainWindow::CheckChanges() {
    auto changes = std::make_shared<std::vector<std::pair<QString, ChangeSummary>>>(); auto mutex = std::make_shared<std::mutex>(); std::vector<GuiTask> tasks;
    for (const auto index : SelectedPackages()) if (const auto workspace = packages_[index].workspace) tasks.push_back({Q(workspace->manifest.packageName), [workspace, changes, mutex](const ArchiveJob& job) {
        auto summary = workspace->CheckChanges(job); std::lock_guard lock(*mutex); changes->emplace_back(Q(workspace->manifest.packageName), std::move(summary));
    }});
    if (tasks.empty()) { ShowError("所选资源包尚未建立编辑工作区。替换、新增或打开解包清单后可以检查变更。"); return; }
    RunTasks("检查变更", std::move(tasks), [this, changes](const JobResult& result) { if (result.Succeeded()) ShowChanges(*changes); });
}
void MainWindow::PackWorkspaces() {
    auto workspaces = std::make_shared<std::vector<std::shared_ptr<ArchiveWorkspace>>>();
    for (const auto index : SelectedPackages()) if (packages_[index].workspace) workspaces->push_back(packages_[index].workspace);
    if (workspaces->empty()) {
        const auto paths = QFileDialog::getOpenFileNames(this, "选择一个或多个工作区清单", {}, "工作区清单 (WorkspaceManifest.json)");
        if (paths.isEmpty()) return;
        try { for (const auto& path : paths) workspaces->push_back(std::make_shared<ArchiveWorkspace>(ArchiveWorkspace::Load(P(QFileInfo(path).absolutePath())))); }
        catch (const std::exception& error) { ShowError(Q(error.what())); return; }
    }
    auto changes = std::make_shared<std::vector<std::pair<QString, ChangeSummary>>>(); auto mutex = std::make_shared<std::mutex>(); std::vector<GuiTask> tasks;
    for (const auto& workspace : *workspaces) tasks.push_back({Q(workspace->manifest.packageName), [workspace, changes, mutex](const ArchiveJob& job) {
        auto summary = workspace->CheckChanges(job); std::lock_guard lock(*mutex); changes->emplace_back(Q(workspace->manifest.packageName), std::move(summary));
    }});
    if (workspaces->size() > 1) tasks.push_back({"跨包路径检查", [workspaces, changes, mutex](const ArchiveJob& job) {
        std::vector<NamedResourcePath> mounted;
        for (const auto& workspace : *workspaces) for (const auto& entry : workspace->manifest.entries) if (entry.change != ChangeKind::Delete)
            mounted.push_back({Detail::PathUtf8(workspace->root), entry.path});
        ChangeSummary summary;
        for (const auto& collision : FindResourcePathConflicts(mounted, job)) summary.conflicts.push_back("跨包重复资源：" + collision.lookupPath);
        std::lock_guard lock(*mutex); changes->emplace_back("跨包路径检查", std::move(summary));
    }});
    RunTasks("封包前检查", std::move(tasks), [this, workspaces, changes](const JobResult& result) {
        if (!result.Succeeded()) return;
        ShowChanges(*changes, [this, workspaces] {
            QString directory; bool overwrite = false; if (!ChooseOutput(this, directory, overwrite, "生成并验证资源包")) return;
            std::set<std::string> names;
            for (const auto& workspace : *workspaces) if (!names.insert(NormalizePath(workspace->manifest.packageName)).second) { ShowError("多个工作区使用相同输出包名，请分别生成或在清单中改名。"); return; }
            std::vector<GuiTask> jobs;
            for (const auto& workspace : *workspaces) jobs.push_back({Q(workspace->manifest.packageName), [workspace, directory, overwrite](const ArchiveJob& job) {
                BuildArchive(*workspace, P(directory) / Detail::Utf8Path(workspace->manifest.packageName), {overwrite, {}}, job);
            }});
            RunTasks("生成并验证资源包", std::move(jobs));
        });
    });
}
void MainWindow::VerifyPackages() {
    std::vector<GuiTask> tasks;
    for (const auto index : SelectedPackages()) { const auto archive = packages_[index].archive; if (!archive->source.empty()) tasks.push_back({Q(archive->packageName), [archive](const ArchiveJob& job) { ValidateArchive(archive->source, {archive->logicalDirectory, archive->packageName}, job); }}); }
    if (tasks.empty()) { ShowError("请先打开实际资源包。"); return; }
    RunTasks("完整验证资源包", std::move(tasks));
}
void MainWindow::CreatePackage() {
    QDialog dialog(this); dialog.setWindowTitle("从目录新建 PKN"); dialog.resize(660, 270); auto* layout = new QVBoxLayout(&dialog); auto* form = new QFormLayout; layout->addLayout(form);
    auto* input = new QLineEdit; auto* namespaceInput = new QLineEdit("Images"); auto* name = new QLineEdit("Custom_00000.pkn");
    auto* workspaceRoot = new QLineEdit(F(DefaultWorkspaceRoot() / "Custom_00000")); auto* mode = new QComboBox;
    for (unsigned i = 0; i <= 4; ++i) mode->addItem(Storage(i), i); mode->setCurrentIndex(3);
    auto* row = new QHBoxLayout; auto* browse = new QPushButton("选择目录…"); row->addWidget(input); row->addWidget(browse); form->addRow("输入目录", row);
    form->addRow("资源命名空间", namespaceInput); form->addRow("客户端包名", name); form->addRow("编辑工作区", workspaceRoot); form->addRow("存储模式", mode);
    connect(browse, &QPushButton::clicked, &dialog, [&] { const auto selected = QFileDialog::getExistingDirectory(&dialog, "选择输入目录"); if (!selected.isEmpty()) input->setText(selected); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("准备工作区"); buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto directory = input->text(), ns = namespaceInput->text(), packageName = name->text(), root = workspaceRoot->text(); const auto flags = mode->currentData().toUInt();
    auto holder = std::make_shared<std::shared_ptr<ArchiveWorkspace>>();
    RunTasks("新建资源工作区", {{packageName, [directory, ns, packageName, root, flags, holder](const ArchiveJob& job) {
        *holder = std::make_shared<ArchiveWorkspace>(ArchiveWorkspace::FromDirectory(P(directory), U(ns), U(packageName), P(root), flags, job));
    }}}, [this, holder](const JobResult& result) {
        if (result.Succeeded() && *holder) { auto archive = std::make_shared<ArchiveCatalog>(); archive->packageName = (*holder)->manifest.packageName; packages_.push_back({archive, *holder}); RebuildCatalog(); }
    });
}
void MainWindow::RecordSmokeStage(const QString& stage) {
    if (smokeReport_.isEmpty()) return;
    setProperty("smokeStage", stage);
    smokeTimings_.emplace_back(stage, smokeClock_.elapsed());
    Json progress = {{"stage", U(stage)}, {"timings_ms", Json::array()}};
    for (const auto& [name, elapsed] : smokeTimings_) progress["timings_ms"].push_back({{"stage", U(name)}, {"elapsed_ms", elapsed}});
    Detail::WriteTextAtomic(P(smokeReport_ + ".progress.json"), progress.dump(2) + "\n");
}
void MainWindow::ConfigureSmokeTest(QString screenshot, QString report) {
    smokeScreenshot_ = std::move(screenshot); smokeReport_ = std::move(report);
    smokeClock_.start(); smokeTimings_.clear(); RecordSmokeStage("opening");
    setProperty("smokeBusyTicks", 0);
    auto* timer = new QTimer(this); timer->setInterval(30);
    connect(timer, &QTimer::timeout, this, [this] { if (busy_) setProperty("smokeBusyTicks", property("smokeBusyTicks").toInt() + 1); }); timer->start();
    QTimer::singleShot(120000, this, [this] { if (!smokeReport_.isEmpty()) {
        const Json report = {{"passed", false}, {"error", "GUI smoke timeout"}, {"stage", U(property("smokeStage").toString())},
                             {"packages", packages_.size()}, {"entries", model_->rows.size()}};
        Detail::WriteTextAtomic(P(smokeReport_), report.dump(2) + "\n"); QCoreApplication::exit(1);
    } });
}
void MainWindow::RunSmokeTest() {
    RecordSmokeStage("search_and_preview");
    Json report = {{"packages", packages_.size()}, {"entries", model_->rows.size()}, {"busy_timer_ticks", property("smokeBusyTicks").toInt()}};
    const auto broadStarted = smokeClock_.elapsed(); search_->setText("s"); QTest::qWait(250);
    report["broad_search_results"] = proxy_->rowCount(); report["broad_search_ms"] = smokeClock_.elapsed() - broadStarted;
    RecordSmokeStage("broad_search_ready");
    search_->clear(); search_->setFocus(); QTest::keyClicks(search_, "Shader/Shader.vsh");
    QTest::qWait(250);
    RecordSmokeStage("search_ready");
    report["search_results"] = proxy_->rowCount();
    if (proxy_->rowCount()) table_->selectRow(0);
    QTimer::singleShot(500, this, [this, report]() mutable {
        report["text_preview_ready"] = !textPreview_->toPlainText().isEmpty(); report["hex_preview_ready"] = hexPreview_->toPlainText().startsWith("00000000");
        nextPage_->click();
        QTimer::singleShot(300, this, [this, report]() mutable {
        report["hex_page_ready"] = previewOffset_ == 4096 && hexPreview_->toPlainText().startsWith("00001000");
        RecordSmokeStage("filter_and_sort");
        search_->clear(); QTest::qWait(250); RecordSmokeStage("search_cleared");
        extensions_->setCurrentIndex(std::max(0, extensions_->findData(".png"))); report["png_results"] = proxy_->rowCount();
        RecordSmokeStage("png_ready");
        extensions_->setCurrentIndex(0); RecordSmokeStage("all_extensions_ready");
        table_->sortByColumn(2, Qt::DescendingOrder); RecordSmokeStage("numeric_sort_ready");
        if (proxy_->rowCount()) table_->selectRow(0);
        report["sort_and_selection"] = !SelectedRows().empty();
        if (proxy_->rowCount() > 1) table_->selectionModel()->select(proxy_->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        report["multiple_selection"] = SelectedRows().size() == 2;
        RecordSmokeStage("multiple_selection_ready");
        search_->setText("Shader/Shader.vsh"); QTest::qWait(250); if (proxy_->rowCount()) table_->selectRow(0);
        const auto shader = std::find_if(packages_.begin(), packages_.end(), [](const auto& package) { return package.archive->packageName == "Shader_00000.pkn"; });
        if (shader == packages_.end()) { report["passed"] = false; if (!smokeReport_.isEmpty()) Detail::WriteTextAtomic(P(smokeReport_), report.dump(2)); QCoreApplication::exit(1); return; }
        const auto archive = shader->archive;
        const auto root = P(QFileInfo(smokeReport_).absolutePath()) / ("GuiSmoke" + std::to_string(QCoreApplication::applicationPid()));
        auto outputHash = std::make_shared<std::string>();
        RecordSmokeStage("selected_extraction");
        QTimer::singleShot(0, this, [root] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto* line = dialog->findChild<QLineEdit*>()) { line->setText(F(root / "Selected")); dialog->accept(); }
        });
        ExtractPackages(true, [this, archive, root, outputHash, report](const JobResult& extracted) mutable {
        report["gui_selected_extraction"] = extracted.Succeeded();
        RecordSmokeStage("round_trip");
        RunTasks("GUI 核心操作一致性自检", {{"Shader_00000.pkn", [archive, root, outputHash](const ArchiveJob& job) {
            auto workspace = ArchiveWorkspace::Load(root / "Selected/Shader_00000");
            if (!workspace.manifest.complete || workspace.manifest.entries.size() != 1 || !workspace.manifest.entries[0].extracted)
                throw ArchiveError(ErrorCode::Integrity, "GUI 选择解包没有生成完整 Shader 清单");
            const auto before = ReadEntryTo(*archive, 0, {}, job).sha256;
            if (HashFile(workspace.InputFile(workspace.manifest.entries[0]), job).sha256 != before)
                throw ArchiveError(ErrorCode::Integrity, "GUI 选择解包内容不同");
            ExportEntry(*archive, 0, root / "Shader.vsh", false, job);
            if (HashFile(root / "Shader.vsh", job).sha256 != before) throw ArchiveError(ErrorCode::Integrity, "GUI 导出结果不同");
            BuildArchive(workspace, root / "Shader_00000.pkn", {}, job);
            const auto rebuilt = OpenArchive(root / "Shader_00000.pkn", {}, job);
            if (ReadEntryTo(rebuilt, 0, {}, job).sha256 != before) throw ArchiveError(ErrorCode::Integrity, "GUI 重封结果不同");
            *outputHash = HashFile(root / "Shader_00000.pkn", job).sha256;
        }}}, [this, report, outputHash](const JobResult& result) mutable {
            report["gui_core_round_trip"] = result.Succeeded(); report["rebuilt_sha256"] = *outputHash;
            report["busy_timer_ticks"] = property("smokeBusyTicks").toInt();
            RecordSmokeStage("complete"); report["timings_ms"] = Json::array();
            for (const auto& [name, elapsed] : smokeTimings_) report["timings_ms"].push_back({{"stage", U(name)}, {"elapsed_ms", elapsed}});
#ifdef _DEBUG
            constexpr bool requireFilterPerformance = false;
#else
            constexpr bool requireFilterPerformance = true;
#endif
            const bool filterPerformanceGood = report["broad_search_ms"].get<qint64>() < 5000;
            report["filter_performance"] = {{"threshold_ms", 5000}, {"is_acceptance_criterion", requireFilterPerformance}, {"passed", filterPerformanceGood}};
            report["passed"] = result.Succeeded() && report["gui_selected_extraction"].get<bool>() && report["search_results"] == 1 && report["png_results"] == 22134
                && report["broad_search_results"] == 164222 && (!requireFilterPerformance || filterPerformanceGood)
                && report["text_preview_ready"].get<bool>() && report["hex_preview_ready"].get<bool>() && report["hex_page_ready"].get<bool>()
                && report["sort_and_selection"].get<bool>() && report["multiple_selection"].get<bool>() && property("smokeBusyTicks").toInt() > 0;
            if (!smokeScreenshot_.isEmpty()) {
                if (auto* tabs = findChild<QTabWidget*>()) tabs->setCurrentWidget(hexPreview_);
                Detail::EnsureSafeRoot(P(QFileInfo(smokeScreenshot_).absolutePath())); grab().save(smokeScreenshot_);
            }
            if (!smokeReport_.isEmpty()) Detail::WriteTextAtomic(P(smokeReport_), report.dump(2) + "\n");
            const auto passed = report["passed"].get<bool>(); smokeReport_.clear(); QCoreApplication::exit(passed ? 0 : 1);
        });
        });
        });
    });
}
