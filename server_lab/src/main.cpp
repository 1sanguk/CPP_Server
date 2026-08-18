#include <QApplication>
#include <QTimer>
#include <main_window.h>
int main(int argc, char** argv){
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    const QString screenshot_path = qEnvironmentVariable("SERVER_LAB_SCREENSHOT_PATH");
    if(!screenshot_path.isEmpty()){
        QTimer::singleShot(1000, &window, [&app, &window, screenshot_path]{
            const bool saved = window.grab().save(screenshot_path);
            app.exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
