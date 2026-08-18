#pragma once
#include <QMainWindow>
#include <QProcess>
#include <vector>
#include <server_catalog.h>

class QListWidget;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class ClientPanel;
class QCloseEvent;

class MainWindow : public QMainWindow{
public:
    MainWindow();
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void Refresh_Detail();
    void Start_Room();
    void Stop_Room();
    void Build_And_Start(const ServerDescriptor& descriptor);
    void Launch_Native(const ServerDescriptor& descriptor, const QString& executable);
    void Track_Room_Process(QProcess* process);
    QString Find_Executable(const ServerDescriptor& descriptor) const;
    QString Repo_Root() const;
    std::vector<ServerEntry> catalog;
    QListWidget* version_list;
    QLabel* status_label;
    QLabel* detail_label;
    QPlainTextEdit* log_view;
    QPushButton* start_button;
    ClientPanel* client_panel;
    QProcess* room_process = nullptr;
    QString active_container_name;
    bool closing = false;
};
