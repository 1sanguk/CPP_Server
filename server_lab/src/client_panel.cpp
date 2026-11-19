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
#include <QtEndian>
#include <functional>

namespace{
// server/v7/include/packet.h와 같은 값이어야 한다. [uint16 size][uint16 id][body], little-endian.
constexpr int kHeaderSize = 4;
constexpr int kMaxPacketSize = 8 * 1024;
constexpr quint16 kClientChat = 1;
constexpr quint16 kServerWelcome = 101;
constexpr quint16 kServerChat = 102;
constexpr quint16 kServerNotice = 103;

class ClientTab : public QWidget{
public:
    using ChatCallback = std::function<void(int, const QString&)>;
    using StatusCallback = std::function<void(int, bool)>;

    ClientTab(int number, bool packet_protocol, ChatCallback chat_callback, StatusCallback status_callback,
        QWidget* parent = nullptr)
        : QWidget(parent), number(number), packet_protocol(packet_protocol),
          chat_callback(std::move(chat_callback)), status_callback(std::move(status_callback)){
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
            if(this->packet_protocol){
                Read_Packets();
                return;
            }
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
        if(!packet_protocol){
            socket->write(text.toUtf8());
            Append("SEND", text);
            return;
        }

        const QByteArray body = text.toUtf8();
        if(body.size() > kMaxPacketSize - kHeaderSize){
            Append("SEND 실패 — 메시지가 너무 김", text);
            return;
        }
        QByteArray packet(kHeaderSize, Qt::Uninitialized);
        qToLittleEndian<quint16>(static_cast<quint16>(kHeaderSize + body.size()), packet.data());
        qToLittleEndian<quint16>(kClientChat, packet.data() + 2);
        packet.append(body);
        socket->write(packet);
        Append("SEND", text);
    }

    // TCP 스트림에서 완성된 패킷만 잘라 처리한다. 서버의 PacketAssembler와 같은 규칙이다.
    void Read_Packets(){
        recv_buffer.append(socket->readAll());
        while(recv_buffer.size() >= kHeaderSize){
            const int size = qFromLittleEndian<quint16>(recv_buffer.constData());
            if(size < kHeaderSize || size > kMaxPacketSize){
                Append("ERROR", QString("잘못된 패킷 크기 %1 — 연결을 끊습니다").arg(size));
                recv_buffer.clear();
                socket->abort();
                return;
            }
            if(recv_buffer.size() < size) return;
            const quint16 id = qFromLittleEndian<quint16>(recv_buffer.constData() + 2);
            const QByteArray body = recv_buffer.mid(kHeaderSize, size - kHeaderSize);
            recv_buffer.remove(0, size);
            Handle_Packet(id, body);
        }
    }

    void Handle_Packet(quint16 id, const QByteArray& body){
        if(id == kServerWelcome && body.size() >= 4){
            session_id = qFromLittleEndian<quint32>(body.constData());
            Append("WELCOME", QString("Session %1로 입장").arg(session_id));
        }else if(id == kServerChat && body.size() >= 4){
            const quint32 sender = qFromLittleEndian<quint32>(body.constData());
            const QString text = QString::fromUtf8(body.mid(4));
            Append("RECV", QString("[Session %1] %2").arg(sender).arg(text));
            // 같은 방송을 모든 클라이언트가 받으므로, 공통 채팅에는 발신자가 받은 사본만 한 번 기록한다.
            if(sender == session_id) chat_callback(number, text);
        }else if(id == kServerNotice){
            Append("NOTICE", QString::fromUtf8(body));
        }else{
            Append("ERROR", QString("알 수 없는 패킷 id %1").arg(id));
        }
    }

    void Append(const QString& event, const QString& text = {}){
        const QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
        log->appendPlainText(text.isEmpty()
            ? QString("[%1] %2").arg(time, event)
            : QString("[%1] %2: %3").arg(time, event, text));
    }

    int number;
    bool packet_protocol;
    QByteArray recv_buffer;
    quint32 session_id = 0;
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
    shared_note = new QLabel(shared_panel);
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
    Set_Server_Version(0);
    Set_Room_Active(false);
}

bool ClientPanel::Uses_Packet_Protocol() const{
    return server_version >= 7;
}

QString ClientPanel::Idle_Message() const{
    return Uses_Packet_Protocol()
        ? "먼저 실험방을 시작하세요. v7은 채팅 패킷을 방 전체에 방송합니다."
        : "먼저 실험방을 시작하세요. 각 연결은 Echo 응답을 독립적으로 표시합니다.";
}

void ClientPanel::Set_Server_Version(int version){
    server_version = version;
    shared_note->setText(Uses_Packet_Protocol()
        ? "v7 채팅 서버가 방 전체에 방송한 메시지를 발신자 기준으로 한 번씩 표시합니다. "
          "개별 화면에는 다른 클라이언트의 채팅과 입장/퇴장 알림까지 실제로 받은 패킷이 모두 표시됩니다."
        : "서버의 Echo 응답이 도착한 메시지만 이곳에 모아 표시합니다. 개별 화면에는 실제 SEND/RECV만 표시됩니다.");
    if(!room_active) state_label->setText(Idle_Message());
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
        state_label->setText(Idle_Message());
    }
}

void ClientPanel::Create_Clients(){
    if(!room_active) return;
    Disconnect_Clients();
    shared_chat->clear();
    for(int i = 1; i <= client_count->value(); ++i){
        auto* client = new ClientTab(i, Uses_Packet_Protocol(), [this](int sender, const QString& text){
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
