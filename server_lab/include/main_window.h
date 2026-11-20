#pragma once
#include <QMainWindow>
#include <QProcess>
#include <vector>
#include <server_catalog.h>

class QCheckBox;
class QListWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class ClientPanel;
class LogList;
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

    // 검색줄의 조건을 클라이언트 카드와 서버 로그에 적용하고, 어디서 몇 건 찾았는지 표시한다.
    void Apply_Search();
    void Update_Search_Summary();
    // 서버 프로그램 출력(줄 중간에서 잘려 올 수 있음)을 한 줄씩 서버 로그에 넣는다.
    void Append_Server_Log(const QString& text);
    void Flush_Server_Log();
    // Lab 자체 안내(실험방 시작/종료, 빌드 등)를 서버 로그에 구분되게 넣는다.
    void Add_Lab_Message(const QString& text);

    std::vector<ServerEntry> catalog;
    QListWidget* version_list;
    QLabel* status_label;
    QLabel* detail_label;
    LogList* log_view;
    QString server_log_partial;
    QLineEdit* search_edit;
    QCheckBox* search_only;
    QLabel* search_summary;
    QPushButton* start_button;
    ClientPanel* client_panel;
    QProcess* room_process = nullptr;
    QString active_container_name;
    bool closing = false;
};
