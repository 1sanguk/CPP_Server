#pragma once

#include <QWidget>

class QLabel;
class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

class ClientPanel : public QWidget{
public:
    explicit ClientPanel(QWidget* parent = nullptr);
    ~ClientPanel() override;
    void Set_Room_Active(bool active);
    // v7부터는 길이 헤더 패킷 프로토콜을 사용하므로 실험방 버전에 맞춰 클라이언트 동작을 바꾼다.
    void Set_Server_Version(int version);
    void Shutdown();

private:
    bool Uses_Packet_Protocol() const;
    QString Idle_Message() const;

    void Create_Clients();
    void Disconnect_Clients();
    void Broadcast_Chat(int sender, const QString& text);

    QLabel* state_label;
    QSpinBox* client_count;
    QCheckBox* random_clients;
    QPushButton* connect_button;
    QPushButton* disconnect_button;
    QLineEdit* client_search;
    QListWidget* client_list;
    QStackedWidget* client_pages;
    QPlainTextEdit* shared_chat;
    QLabel* shared_note;
    int server_version = 0;
    bool room_active = false;
    bool shutting_down = false;
};
