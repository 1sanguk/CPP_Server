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
    void Shutdown();

private:
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
    bool room_active = false;
    bool shutting_down = false;
};
