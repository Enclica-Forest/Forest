// forb-customizer - a Qt6 Widgets WYSIWYG editor for ForeB's forebo.cfg.
#include <QApplication>
#include <QTimer>
#include <QString>
#include "MainWindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("forb-customizer");
    app.setOrganizationName("ForeB");
    MainWindow w;
    w.resize(1200, 780);
    w.show();
    // Headless capture hook: FORB_SHOT=<path> grabs the window and exits (for
    // offscreen verification). QWidget::grab() renders without a real display.
    const char *shot = qgetenv("FORB_SHOT").constData();
    if (shot && shot[0]) {
        QString path = QString::fromLocal8Bit(shot);
        QTimer::singleShot(400, [&app, &w, path]() {
            w.grab().save(path);
            app.quit();
        });
    }
    return app.exec();
}
