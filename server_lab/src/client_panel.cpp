#include <client_panel.h>

#include <QDateTime>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

namespace{
class ClientTab : public QWidget{
public:
    using ChatCallback = std::function<void(int, const QString&)>;
    using StatusCallback = std::function<void(int, bool)>;

    ClientTab(int number, ChatCallback chat_callback, StatusCallback status_callback, QWidget* parent = nullptr)
        : QWidget(parent), number(number), chat_callback(std::move(chat_callback)),
          status_callback(std::move(status_callback)){
        auto* layout = new QVBoxLayout(this);
        state = new QLabel("연결 중...", this);
        log = new QPlainTextEdit(this);
        log->setReadOnly(true);
        auto* message_row = new QHBoxLayout();
        message = new QLineEdit(this);
        message->setPlaceholderText("서버로 보낼 메시지");
        auto* send = new QPushButton("바로 보내기", this);
        message_row->addWidget(message, 1);
        message_row->addWidget(send);

        auto* delayed_row = new QHBoxLayout();
        delayed_seconds = new QSpinBox(this);
        delayed_seconds->setRange(1, 3600);
        delayed_seconds->setValue(3);
        auto* delayed = new QPushButton("초 뒤 1회 보내기", this);
        delayed_row->addWidget(new QLabel("지연:", this));
        delayed_row->addWidget(delayed_seconds);
        delayed_row->addWidget(delayed);

        auto* repeat_row = new QHBoxLayout();
        repeat_seconds = new QSpinBox(this);
        repeat_seconds->setRange(1, 3600);
        repeat_seconds->setValue(2);
        repeat_button = new QPushButton("초마다 반복 시작", this);
        repeat_row->addWidget(new QLabel("반복:", this));
        repeat_row->addWidget(repeat_seconds);
        repeat_row->addWidget(repeat_button);

        auto* disconnect = new QPushButton("이 클라이언트 연결 종료", this);

        layout->addWidget(state);
        layout->addWidget(log, 1);
        layout->addLayout(message_row);
        layout->addLayout(delayed_row);
        layout->addLayout(repeat_row);
        layout->addWidget(disconnect);

        socket = new QTcpSocket(this);
        repeat_timer = new QTimer(this);
        connect(socket, &QTcpSocket::connected, this, [this]{
            state->setText(QString("Client %1 — 연결됨 (localhost:9000)").arg(this->number));
            Append("연결 성공");
            this->status_callback(this->number, true);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this]{
            state->setText(QString("Client %1 — 연결 종료").arg(this->number));
            repeat_timer->stop();
            repeat_button->setText("초마다 반복 시작");
            Append("연결 종료");
            this->status_callback(this->number, false);
        });
        connect(socket, &QTcpSocket::readyRead, this, [this]{
            const QString echoed = QString::fromUtf8(socket->readAll());
            Append("RECV", echoed);
            this->chat_callback(this->number, echoed);
        });
        connect(socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
            Append("ERROR", socket->errorString());
        });
        connect(send, &QPushButton::clicked, this, [this]{ Send_Current(); });
        connect(message, &QLineEdit::returnPressed, this, [this]{ Send_Current(); });
        connect(delayed, &QPushButton::clicked, this, [this]{
            const QString scheduled = message->text();
            Append(QString("%1초 뒤 전송 예약").arg(delayed_seconds->value()), scheduled);
            QTimer::singleShot(delayed_seconds->value() * 1000, this, [this, scheduled]{ Send(scheduled); });
        });
        connect(repeat_button, &QPushButton::clicked, this, [this]{
            if(repeat_timer->isActive()){
                repeat_timer->stop();
                repeat_button->setText("초마다 반복 시작");
                Append("반복 전송 중지");
            }else{
                repeat_message = message->text();
                if(repeat_message.isEmpty()) return;
                repeat_timer->start(repeat_seconds->value() * 1000);
                repeat_button->setText("반복 중지");
                Append(QString("%1초 간격 반복 시작").arg(repeat_seconds->value()), repeat_message);
            }
        });
        connect(repeat_timer, &QTimer::timeout, this, [this]{ Send(repeat_message); });
        connect(disconnect, &QPushButton::clicked, this, [this]{ Disconnect(); });
        socket->connectToHost("127.0.0.1", 9000);
    }

    void Disconnect(){
        repeat_timer->stop();
        socket->disconnectFromHost();
        if(socket->state() != QAbstractSocket::UnconnectedState) socket->abort();
    }

    void Configure_Random_Automation(){
        static const QStringList messages{
            "hello", "ready", "move north", "attack", "need help",
            "party invite", "item acquired", "ping", "good game", "bye"
        };
        const QString selected = messages.at(QRandomGenerator::global()->bounded(messages.size()));
        const int seconds = QRandomGenerator::global()->bounded(1, 6);
        message->setText(selected);
        if(QRandomGenerator::global()->bounded(2) == 0){
            delayed_seconds->setValue(seconds);
            Append(QString("랜덤 설정: %1초 뒤 1회 전송").arg(seconds), selected);
            QTimer::singleShot(seconds * 1000, this, [this, selected]{ Send(selected); });
        }else{
            repeat_seconds->setValue(seconds);
            repeat_message = selected;
            repeat_timer->start(seconds * 1000);
            repeat_button->setText("반복 중지");
            Append(QString("랜덤 설정: %1초 간격 반복").arg(seconds), selected);
        }
    }

private:
    void Send_Current(){
        const QString text = message->text();
        Send(text);
        if(!text.isEmpty()) message->clear();
    }

    void Send(const QString& text){
        if(text.isEmpty()) return;
        if(socket->state() != QAbstractSocket::ConnectedState){
            Append("SEND 실패 — 연결되지 않음", text);
            return;
        }
        socket->write(text.toUtf8());
        Append("SEND", text);
    }

    void Append(const QString& event, const QString& text = {}){
        const QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
        log->appendPlainText(text.isEmpty()
            ? QString("[%1] %2").arg(time, event)
            : QString("[%1] %2: %3").arg(time, event, text));
    }

    int number;
    QLabel* state;
    QPlainTextEdit* log;
    QLineEdit* message;
    QSpinBox* delayed_seconds;
    QSpinBox* repeat_seconds;
    QPushButton* repeat_button;
    QTcpSocket* socket;
    QTimer* repeat_timer;
    QString repeat_message;
    ChatCallback chat_callback;
    StatusCallback status_callback;
};
}

ClientPanel::ClientPanel(QWidget* parent) : QWidget(parent){
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("클라이언트 테스트", this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    state_label = new QLabel("먼저 실험방을 시작하세요. 각 연결은 Echo 응답을 독립적으로 표시합니다.", this);
    state_label->setWordWrap(true);

    auto* controls = new QHBoxLayout();
    client_count = new QSpinBox(this);
    client_count->setRange(1, 200);
    client_count->setValue(2);
    random_clients = new QCheckBox("랜덤 자동 메시지", this);
    random_clients->setToolTip("각 클라이언트에 임의 메시지와 1~5초 지연/반복 전송을 설정합니다.");
    connect_button = new QPushButton("클라이언트 연결", this);
    disconnect_button = new QPushButton("전체 연결 종료", this);
    controls->addWidget(new QLabel("접속 수:", this));
    controls->addWidget(client_count);
    controls->addWidget(random_clients);
    controls->addWidget(connect_button);
    controls->addWidget(disconnect_button);

    auto* content = new QSplitter(Qt::Horizontal, this);
    auto* shared_panel = new QWidget(content);
    auto* shared_layout = new QVBoxLayout(shared_panel);
    auto* shared_title = new QLabel("Lab 공통 채팅", shared_panel);
    auto* shared_note = new QLabel(
        "서버의 Echo 응답이 도착한 메시지만 이곳에 모아 표시합니다. 개별 화면에는 실제 SEND/RECV만 표시됩니다.", shared_panel);
    shared_note->setWordWrap(true);
    shared_chat = new QPlainTextEdit(shared_panel);
    shared_chat->setReadOnly(true);
    shared_layout->addWidget(shared_title);
    shared_layout->addWidget(shared_note);
    shared_layout->addWidget(shared_chat, 1);
    auto* clients_panel = new QWidget(content);
    auto* clients_layout = new QHBoxLayout(clients_panel);
    auto* navigator = new QWidget(clients_panel);
    auto* navigator_layout = new QVBoxLayout(navigator);
    navigator_layout->setContentsMargins(0, 0, 0, 0);
    client_search = new QLineEdit(navigator);
    client_search->setPlaceholderText("클라이언트 검색");
    client_list = new QListWidget(navigator);
    client_list->setMinimumWidth(130);
    client_list->setMaximumWidth(180);
    navigator_layout->addWidget(client_search);
    navigator_layout->addWidget(client_list, 1);
    client_pages = new QStackedWidget(clients_panel);
    clients_layout->addWidget(navigator);
    clients_layout->addWidget(client_pages, 1);
    content->addWidget(shared_panel);
    content->addWidget(clients_panel);
    content->setStretchFactor(0, 1);
    content->setStretchFactor(1, 1);
    content->setSizes({500, 500});
    layout->addWidget(title);
    layout->addWidget(state_label);
    layout->addLayout(controls);
    layout->addWidget(content, 1);
    connect(connect_button, &QPushButton::clicked, this, [this]{ Create_Clients(); });
    connect(disconnect_button, &QPushButton::clicked, this, [this]{ Disconnect_Clients(); });
    connect(client_list, &QListWidget::currentRowChanged, client_pages, &QStackedWidget::setCurrentIndex);
    connect(client_search, &QLineEdit::textChanged, this, [this](const QString& query){
        for(int i = 0; i < client_list->count(); ++i){
            auto* item = client_list->item(i);
            item->setHidden(!item->text().contains(query, Qt::CaseInsensitive));
        }
    });
    Set_Room_Active(false);
}

ClientPanel::~ClientPanel(){
    Shutdown();
}

void ClientPanel::Shutdown(){
    if(shutting_down) return;
    shutting_down = true;
    room_active = false;
    Disconnect_Clients();
}

void ClientPanel::Set_Room_Active(bool active){
    room_active = active;
    connect_button->setEnabled(active);
    client_count->setEnabled(active);
    random_clients->setEnabled(active);
    if(active){
        state_label->setText("실험방 실행 중 — 접속 수를 정하고 클라이언트를 연결하세요.");
    }else{
        Disconnect_Clients();
        state_label->setText("먼저 실험방을 시작하세요. 각 연결은 Echo 응답을 독립적으로 표시합니다.");
    }
}

void ClientPanel::Create_Clients(){
    if(!room_active) return;
    Disconnect_Clients();
    shared_chat->clear();
    for(int i = 1; i <= client_count->value(); ++i){
        auto* client = new ClientTab(i, [this](int sender, const QString& text){
            Broadcast_Chat(sender, text);
        }, [this](int client_number, bool connected){
            auto* item = client_list->item(client_number - 1);
            if(item) item->setText(QString("%1 Client %2").arg(connected ? "●" : "○").arg(client_number));
        }, client_pages);
        client_pages->addWidget(client);
        client_list->addItem(QString("○ Client %1").arg(i));
        if(random_clients->isChecked()) client->Configure_Random_Automation();
    }
    if(client_list->count() > 0) client_list->setCurrentRow(0);
    disconnect_button->setEnabled(true);
}

void ClientPanel::Disconnect_Clients(){
    while(client_pages->count() > 0){
        auto* client = static_cast<ClientTab*>(client_pages->widget(0));
        client_pages->removeWidget(client);
        client->Disconnect();
        delete client;
    }
    client_list->clear();
    client_search->clear();
    disconnect_button->setEnabled(false);
}

void ClientPanel::Broadcast_Chat(int sender, const QString& text){
    if(shutting_down) return;
    const QString line = QString("[Client %1]: %2").arg(sender).arg(text);
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    shared_chat->appendPlainText(QString("[%1] %2").arg(time, line));
}
