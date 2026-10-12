#include "MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFont>
#include <QStyleFactory>
#include <QLibraryInfo>
#include <QTranslator>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("PKNPacker"); app.setApplicationVersion("1.0.0");
    app.setOrganizationName("PKNPacker");
    QTranslator chinese;
    if (chinese.load("qtbase_zh_CN", QCoreApplication::applicationDirPath() + "/translations") ||
        chinese.load("qtbase_zh_CN", QLibraryInfo::path(QLibraryInfo::TranslationsPath))) app.installTranslator(&chinese);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setFont(QFont("Microsoft YaHei UI", 9));
    QFile theme(":/PKNPacker/Theme.css");
    if (theme.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(theme.readAll()));
    QCommandLineParser parser;
    parser.setApplicationDescription("BubbleFighter PKN 资源管理器"); parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"open", "打开 PKN 或扫描目录", "path"});
    parser.addOption({"workspace", "打开工作区", "path"});
    parser.addOption({"smoke-test", "运行界面自检后退出"});
    parser.addOption({"screenshot", "自检截图输出", "path"});
    parser.addOption({"report", "自检结果输出", "path"});
    parser.addPositionalArgument("pkn", "要打开的 PKN 文件", "[pkn...]");
    parser.process(app);
    MainWindow window;
    window.show();
    if (parser.isSet("smoke-test")) window.ConfigureSmokeTest(parser.value("screenshot"), parser.value("report"));
    if (parser.isSet("workspace")) window.OpenWorkspaces(parser.values("workspace"));
    else {
        auto paths = parser.positionalArguments(); paths.append(parser.values("open"));
        if (!paths.isEmpty()) window.OpenPaths(paths);
    }
    return app.exec();
}
