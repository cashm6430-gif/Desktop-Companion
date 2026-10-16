// Renders the pet's tray menu with the shared stylesheet to a PNG, so the look
// (padding, colors, and especially the submenu arrow) can be inspected without
// a live tray icon. Offscreen-safe:
//
//     TrayMenuPreview <out.png>
//
// Run with QT_QPA_PLATFORM=offscreen so it never flashes a window.
#include <QApplication>
#include <QMenu>
#include <QPixmap>
#include <QString>

#include "../src/TrayMenuStyle.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMenu root;
    root.setStyleSheet(tray_style::menuStyleSheet());
    root.addAction(QStringLiteral("显示 / 隐藏  (Ctrl+Alt+H)"));
    root.addSeparator();
    QMenu* look = root.addMenu(QStringLiteral("互动"));
    look->addAction(QStringLiteral("打个招呼（挥手）"));
    look->addAction(QStringLiteral("摸摸头"));
    look->addAction(QStringLiteral("躲一下"));
    look->addAction(QStringLiteral("眯一会儿（眼罩）"));
    look->addAction(QStringLiteral("放个便笺…"));
    QMenu* work = root.addMenu(QStringLiteral("工作与休息"));
    work->addAction(QStringLiteral("歇一下（伸展偷看）"));
    work->addAction(QStringLiteral("结束忙碌（复位）"));
    QMenu* settings = root.addMenu(QStringLiteral("设置"));
    settings->addMenu(QStringLiteral("自发行为"))->addAction(QStringLiteral("正常"));
    settings->addMenu(QStringLiteral("性能"))->addAction(QStringLiteral("标准（~25 fps）"));
    QMenu* preview = root.addMenu(QStringLiteral("预览（开发用）"));
    preview->addAction(QStringLiteral("抱电脑工作"));
    QAction* checkable = preview->addAction(QStringLiteral("新互动（待审批）"));
    checkable->setCheckable(true);
    checkable->setChecked(true);
    root.addSeparator();
    root.addAction(QStringLiteral("退出"));

    root.setAttribute(Qt::WA_DontShowOnScreen, true);
    root.show();
    app.processEvents();

    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("tray-menu.png");
    return root.grab().save(out) ? 0 : 1;
}
