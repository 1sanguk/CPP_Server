#include <client_panel.h>
#include <log_list.h>

#include <QDateTime>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QPainter>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>

namespace{
// server/v7~v8/include/packet.h와 같은 값이어야 한다. [uint16 size][uint16 id][body], little-endian.
constexpr int kHeaderSize = 4;
constexpr int kMaxPacketSize = 8 * 1024;
constexpr quint16 kClientChat = 1;
constexpr quint16 kClientMove = 2;
constexpr quint16 kServerWelcome = 101;
constexpr quint16 kServerChat = 102;
constexpr quint16 kServerNotice = 103;
// v8 월드 패킷
constexpr quint16 kServerEnterWorld = 104;
constexpr quint16 kServerSpawn = 105;
constexpr quint16 kServerDespawn = 106;
constexpr quint16 kServerMoveBatch = 107;
constexpr quint16 kServerSelfState = 108;
// GUI 이동 버튼이 요청하는 초당 이동 거리. 서버 상한(기본 60)을 넘으면 서버가 줄인다.
constexpr float kGuiMoveSpeed = 60.0f;

float Read_Float(const char* data){
    return std::bit_cast<float>(qFromLittleEndian<quint32>(data));
}

QByteArray Make_Packet(quint16 id, const QByteArray& body){
    QByteArray packet(kHeaderSize, Qt::Uninitialized);
    qToLittleEndian<quint16>(static_cast<quint16>(kHeaderSize + body.size()), packet.data());
    qToLittleEndian<quint16>(id, packet.data() + 2);
    packet.append(body);
    return packet;
}

// v8 월드를 위에서 내려다본 미니맵. 격자, 내 시야(3 × 3 셀), 나와 주변 플레이어를 그린다.
// 서버가 보내 준 것만 그리므로 시야 밖 플레이어는 보이지 않는다(AOI가 동작하는지 눈으로 확인하는 용도).
class WorldMap : public QWidget{
public:
    explicit WorldMap(const QHash<quint32, QPointF>* others, QWidget* parent = nullptr)
        : QWidget(parent), others(others){
        setMinimumSize(170, 170);
    }

    void Set_World(float world, float cell){
        world_size = world;
        cell_size = cell;
        update();
    }

    void Set_Self(QPointF position){
        self = position;
        has_self = true;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override{
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const double side = std::min(width(), height()) - 2.0;
        painter.fillRect(QRectF(1, 1, side, side), QColor(32, 36, 44));
        if(!(world_size > 0.0f) || !(cell_size > 0.0f)){
            return;
        }
        const double scale = side / world_size;
        const auto to_screen = [scale](QPointF world){ return QPointF(1 + world.x() * scale, 1 + world.y() * scale); };

        // 내 시야(자기 셀 + 주변 8칸)
        if(has_self){
            const int columns = std::max(1, static_cast<int>(std::ceil(world_size / cell_size)));
            const int cx = std::clamp(static_cast<int>(self.x() / cell_size), 0, columns - 1);
            const int cy = std::clamp(static_cast<int>(self.y() / cell_size), 0, columns - 1);
            const double left = std::max(0, cx - 1) * cell_size;
            const double top = std::max(0, cy - 1) * cell_size;
            const double right = std::min(static_cast<double>(world_size), (cx + 2) * static_cast<double>(cell_size));
            const double bottom = std::min(static_cast<double>(world_size), (cy + 2) * static_cast<double>(cell_size));
            painter.fillRect(QRectF(to_screen({left, top}), to_screen({right, bottom})), QColor(70, 110, 160, 90));
        }
        // 셀 격자
        painter.setPen(QColor(60, 66, 78));
        for(double line = cell_size; line < world_size; line += cell_size){
            painter.drawLine(to_screen({line, 0}), to_screen({line, world_size}));
            painter.drawLine(to_screen({0, line}), to_screen({world_size, line}));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(110, 190, 255));
        for(const QPointF& position : *others){
            painter.drawEllipse(to_screen(position), 2.5, 2.5);
        }
        if(has_self){
            painter.setBrush(QColor(255, 120, 90));
            painter.drawEllipse(to_screen(self), 4.0, 4.0);
        }
    }

private:
    const QHash<quint32, QPointF>* others;
    float world_size = 0.0f;
    float cell_size = 0.0f;
    QPointF self;
    bool has_self = false;
};

const QStringList& Random_Messages(){
    static const QStringList messages{
        "안녕하세요", "hello", "ready", "파티 구해요", "attack!", "need help",
        "아이템 획득", "ping", "good game", "잠깐만요", "bye"
    };
    return messages;
}

QString Random_Message(){
    const QStringList& messages = Random_Messages();
    return messages.at(QRandomGenerator::global()->bounded(messages.size()));
}

// 클라이언트 하나를 나타내는 카드. 맨 위에 이름·상태·보냄/받음 수를, 아래에 주고받은 내용을 색으로 구분해 보여 준다.
// v8이면 미니맵도 함께 그린다. 조작은 패널 상단에서 하고, 카드 자체에는 버튼을 두지 않는다.
class ClientCard : public QFrame{
public:
    // 서버가 붙인 Session 번호를 "Client N" 같은 카드 이름으로 바꿔 준다(모르면 빈 문자열).
    using NameResolver = std::function<QString(quint32)>;

    ClientCard(int number, bool packet_protocol, bool world_protocol, NameResolver name_of, QWidget* parent = nullptr)
        : QFrame(parent), number(number), packet_protocol(packet_protocol), world_protocol(world_protocol),
          name_of(std::move(name_of)){
        setObjectName("clientCard");
        setMinimumHeight(world_protocol ? 330 : 240);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);
        title = new QLabel(this);
        title->setTextFormat(Qt::RichText);
        // 보낸 것과 받은 것을 왼쪽/오른쪽 칸으로 나눠 한눈에 구분되게 한다.
        sent_list = new LogList(2000, this);
        received_list = new LogList(2000, this);
        auto* logs = new QGridLayout();
        logs->setHorizontalSpacing(8);
        logs->setVerticalSpacing(2);
        auto* sent_title = new QLabel("<b style='color:#1f5fa8'>→ 보낸 메시지</b>", this);
        auto* received_title = new QLabel("<b style='color:#2a8a3e'>← 받은 메시지</b> <span style='color:#888'>· 알림</span>", this);
        logs->addWidget(sent_title, 0, 0);
        logs->addWidget(received_title, 0, 1);
        logs->addWidget(sent_list, 1, 0);
        logs->addWidget(received_list, 1, 1);
        // 받는 쪽은 보낸 사람 이름과 알림까지 들어가 줄이 길어서 조금 더 넓게 준다.
        logs->setColumnStretch(0, 2);
        logs->setColumnStretch(1, 3);
        logs->setRowStretch(1, 1);
        layout->addWidget(title);
        if(world_protocol){
            auto* world_row = new QHBoxLayout();
            world_map = new WorldMap(&others, this);
            // 미니맵은 정사각형으로 고정하고, 위치/시야 인원 글자를 바로 옆에 붙인다.
            world_map->setFixedSize(150, 150);
            world_status = new QLabel("위치: -", this);
            world_status->setTextFormat(Qt::RichText);
            world_row->addWidget(world_map);
            world_row->addSpacing(8);
            world_row->addWidget(world_status);
            world_row->addStretch(1);
            layout->addLayout(world_row);
        }
        layout->addLayout(logs, 1);

        socket = new QTcpSocket(this);
        auto_timer = new QTimer(this);
        auto_timer->setSingleShot(true);
        connect(socket, &QTcpSocket::connected, this, [this]{ Log(Kind::Info, "서버에 연결됨"); Refresh(); });
        connect(socket, &QTcpSocket::disconnected, this, [this]{
            auto_timer->stop();
            Log(Kind::Info, "연결 종료");
            Refresh();
        });
        connect(socket, &QTcpSocket::readyRead, this, [this]{
            if(this->packet_protocol){
                Read_Packets();
                return;
            }
            // v6 이하 Echo: 보낸 내용이 그대로 돌아온다.
            Mark_Received();
            Log(Kind::Received, QString::fromUtf8(socket->readAll()), "돌아옴");
            Refresh();
        });
        connect(socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
            Log(Kind::Error, socket->errorString());
        });
        // 자동 보내기: 1~3초 사이 임의 간격으로 임의 메시지를 계속 보낸다.
        connect(auto_timer, &QTimer::timeout, this, [this]{
            Send_Random();
            Schedule_Auto();
        });
        Refresh();
        socket->connectToHost("127.0.0.1", 9000);
    }

    bool Connected() const{ return socket->state() == QAbstractSocket::ConnectedState; }

    // 두 칸(보냄/받음)에 같은 검색 조건을 적용하고 일치한 줄 수를 돌려준다.
    int Apply_Search(const QString& query, bool only_matches){
        const int matches = sent_list->Apply_Search(query, only_matches) + received_list->Apply_Search(query, only_matches);
        setProperty("matches", matches);
        return matches;
    }

    // 검색 중에 새로 들어온 줄까지 포함한 현재 일치 수
    int Match_Count() const{ return sent_list->Match_Count() + received_list->Match_Count(); }
    int Sent() const{ return sent_count; }
    int Received() const{ return received_count; }
    quint32 Session_Id() const{ return session_id; }
    QString Name() const{ return QString("Client %1").arg(number); }

    // 제목 줄과 테두리 색을 갱신한다. 연결돼 있는데 5초 넘게 아무것도 못 받았으면 테두리를 주황색으로
    // 칠해 "이 클라이언트만 조용하다"는 걸 바로 보이게 한다.
    void Refresh(){
        const bool connected = Connected();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool quiet = connected && sent_count > 0 && (last_received_ms == 0 || now - last_received_ms > kQuietMs);
        const QString state = connected
            ? "<span style='color:#2a8a3e'>● 연결됨</span>"
            : (socket->state() == QAbstractSocket::UnconnectedState ? "<span style='color:#999'>○ 끊김</span>"
                                                                     : "<span style='color:#999'>… 연결 중</span>");
        // 다른 카드 로그와 서버 로그에는 Session 번호가 찍히므로 함께 보여 준다.
        const QString session = session_id == 0 ? QString() : QString(" <span style='color:#888'>Session %1</span>").arg(session_id);
        QString text = QString("<span style='font-size:16px; font-weight:bold'>%1</span>%2 &nbsp; %3"
                               " &nbsp;&nbsp; 보냄 <b>%4</b> · 받음 <b>%5</b>")
            .arg(Name(), session, state).arg(sent_count).arg(received_count);
        if(auto_timer->isActive()) text += " &nbsp; <span style='color:#c9433f'>▶ 자동 보내는 중</span>";
        if(title->text() != text) title->setText(text);
        // 오프스크린 GUI 테스트가 화면 문구를 파싱하지 않고 상태를 읽을 수 있게 속성으로도 남긴다.
        setProperty("connected", connected);
        setProperty("sent", sent_count);
        setProperty("received", received_count);
        setProperty("nearby", world_protocol ? static_cast<int>(others.size()) : -1);
        setProperty("autoSending", auto_timer->isActive());
        if(quiet != showing_quiet || !style_initialized){
            showing_quiet = quiet;
            style_initialized = true;
            setStyleSheet(quiet ? "#clientCard{background:#fff8ef; border:2px solid #e08c28; border-radius:10px;}"
                                : "#clientCard{background:white; border:1px solid #d5dbe1; border-radius:10px;}");
            setToolTip(quiet ? "연결돼 있지만 5초 넘게 아무것도 받지 못했습니다." : QString());
        }
    }

    void Send(const QString& text){
        if(text.isEmpty()) return;
        if(!Connected()){
            Log(Kind::Error, "연결되지 않아 보내지 못함: " + text);
            return;
        }
        if(!packet_protocol){
            socket->write(text.toUtf8());
        }else{
            const QByteArray body = text.toUtf8();
            if(body.size() > kMaxPacketSize - kHeaderSize){
                Log(Kind::Error, "메시지가 너무 길어 보내지 못함");
                return;
            }
            socket->write(Make_Packet(kClientChat, body));
        }
        ++sent_count;
        Log(Kind::Sent, text);
        Refresh();
    }

    void Send_Random(){ Send(Random_Message()); }

    void Start_Auto(){
        Schedule_Auto();
        Refresh();
    }

    void Stop_Auto(){
        auto_timer->stop();
        Refresh();
    }

    // v8: 무작위 방향으로 계속 움직여 시야 진입/이탈이 일어나게 한다.
    void Random_Move(){
        const double angle = QRandomGenerator::global()->generateDouble() * 2.0 * 3.14159265358979;
        Send_Move(kGuiMoveSpeed * static_cast<float>(std::cos(angle)), kGuiMoveSpeed * static_cast<float>(std::sin(angle)));
    }

    void Send_Move(float vx, float vy){
        if(!world_protocol || !Connected()) return;
        QByteArray body(12, Qt::Uninitialized);
        qToLittleEndian<quint32>(++input_seq, body.data());
        qToLittleEndian<quint32>(std::bit_cast<quint32>(vx), body.data() + 4);
        qToLittleEndian<quint32>(std::bit_cast<quint32>(vy), body.data() + 8);
        socket->write(Make_Packet(kClientMove, body));
        Log(Kind::Info, vx == 0.0f && vy == 0.0f ? QString("이동 정지") : QString("이동 시작"));
    }

    void Disconnect(){
        auto_timer->stop();
        socket->disconnectFromHost();
        if(socket->state() != QAbstractSocket::UnconnectedState) socket->abort();
    }

private:
    enum class Kind{ Sent, Received, Info, Error };
    static constexpr qint64 kQuietMs = 5000;

    void Schedule_Auto(){
        auto_timer->start(QRandomGenerator::global()->bounded(1000, 3001));
    }

    void Mark_Received(){
        ++received_count;
        last_received_ms = QDateTime::currentMSecsSinceEpoch();
    }

    // Session 번호를 카드 이름으로 바꾼다. 다른 프로세스 클라이언트라 이름을 모르면 Session 번호를 그대로 쓴다.
    QString Display_Name(quint32 session) const{
        const QString name = name_of ? name_of(session) : QString();
        return name.isEmpty() ? QString("Session %1").arg(session) : name;
    }

    // 서버 알림 문장 속 "Session N"을 카드 이름으로 바꿔 읽기 쉽게 한다.
    QString Replace_Session_Names(QString text) const{
        static const QRegularExpression pattern("Session (\\d+)");
        QRegularExpressionMatchIterator it = pattern.globalMatch(text);
        QList<QPair<QString, QString>> replacements;
        while(it.hasNext()){
            const QRegularExpressionMatch match = it.next();
            replacements.push_back({match.captured(0), Display_Name(match.captured(1).toUInt())});
        }
        for(const auto& [from, to] : replacements) text.replace(from, to);
        return text;
    }

    // S_Spawn/S_MoveBatch 항목: uint32 id + 월드 크기 기준 16비트 좌표 2개
    void Read_Entities(const QByteArray& body, bool spawn){
        const int count = qFromLittleEndian<quint16>(body.constData());
        if(body.size() != 2 + count * 8) return;
        for(int i = 0; i < count; ++i){
            const char* entry = body.constData() + 2 + i * 8;
            const quint32 id = qFromLittleEndian<quint32>(entry);
            if(!spawn && !others.contains(id)) continue;
            const double x = qFromLittleEndian<quint16>(entry + 4) * static_cast<double>(world_size) / 65535.0;
            const double y = qFromLittleEndian<quint16>(entry + 6) * static_cast<double>(world_size) / 65535.0;
            others.insert(id, QPointF(x, y));
        }
        if(world_map) world_map->update();
    }

    void Update_World_Status(){
        if(!world_status) return;
        world_status->setText(QString("<span style='color:#888'>내 위치</span><br><b>(%1, %2)</b><br><br>"
                                      "<span style='color:#888'>시야 안</span><br><b>%3명</b>")
            .arg(self_position.x(), 0, 'f', 0).arg(self_position.y(), 0, 'f', 0).arg(others.size()));
    }

    // TCP 스트림에서 완성된 패킷만 잘라 처리한다. 서버의 PacketAssembler와 같은 규칙이다.
    void Read_Packets(){
        recv_buffer.append(socket->readAll());
        while(recv_buffer.size() >= kHeaderSize){
            const int size = qFromLittleEndian<quint16>(recv_buffer.constData());
            if(size < kHeaderSize || size > kMaxPacketSize){
                Log(Kind::Error, QString("잘못된 패킷 크기 %1 — 연결을 끊습니다").arg(size));
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
            Log(Kind::Info, QString("입장 완료 (서버 번호 Session %1)").arg(session_id));
            Refresh();
        }else if(id == kServerChat && body.size() >= 4){
            const quint32 sender = qFromLittleEndian<quint32>(body.constData());
            const QString text = QString::fromUtf8(body.mid(4));
            Mark_Received();
            Log(Kind::Received, text, sender == session_id ? QString("내 메시지") : Display_Name(sender));
            Refresh();
        }else if(id == kServerNotice){
            Log(Kind::Info, Replace_Session_Names(QString::fromUtf8(body)));
        }else if(id == kServerEnterWorld && body.size() == 20){
            // v8: 내 id와 서버가 정한 시작 좌표
            session_id = qFromLittleEndian<quint32>(body.constData());
            const float x = Read_Float(body.constData() + 4);
            const float y = Read_Float(body.constData() + 8);
            world_size = Read_Float(body.constData() + 12);
            const float cell_size = Read_Float(body.constData() + 16);
            self_position = QPointF(x, y);
            if(world_map){
                world_map->Set_World(world_size, cell_size);
                world_map->Set_Self(self_position);
            }
            Update_World_Status();
            Log(Kind::Info, QString("월드 입장 (서버 번호 Session %1)").arg(session_id));
            Refresh();
        }else if(id == kServerSelfState && body.size() == 12){
            // 서버가 계산한 내 위치. 20Hz로 오므로 로그에는 남기지 않는다.
            self_position = QPointF(Read_Float(body.constData() + 4), Read_Float(body.constData() + 8));
            if(world_map) world_map->Set_Self(self_position);
            Update_World_Status();
        }else if(id == kServerSpawn && body.size() >= 2){
            const int count = qFromLittleEndian<quint16>(body.constData());
            Read_Entities(body, true);
            Update_World_Status();
            Log(Kind::Info, QString("%1명이 시야에 들어옴 (시야 안 %2명)").arg(count).arg(others.size()));
        }else if(id == kServerDespawn && body.size() >= 2){
            const int count = qFromLittleEndian<quint16>(body.constData());
            if(body.size() == 2 + count * 4){
                for(int i = 0; i < count; ++i){
                    others.remove(qFromLittleEndian<quint32>(body.constData() + 2 + i * 4));
                }
            }
            if(world_map) world_map->update();
            Update_World_Status();
            Log(Kind::Info, QString("%1명이 시야에서 나감 (시야 안 %2명)").arg(count).arg(others.size()));
        }else if(id == kServerMoveBatch && body.size() >= 2){
            // 이동은 20Hz로 와서 로그에 남기지 않고 미니맵만 갱신한다.
            Read_Entities(body, false);
        }else{
            Log(Kind::Error, QString("알 수 없는 패킷 id %1").arg(id));
        }
    }

    // 로그 한 줄. 보낸 것은 왼쪽 칸(파랑), 받은 것은 오른쪽 칸(초록)에, 알림(회색)과 오류(빨강)도 오른쪽 칸에 쓴다.
    // label은 받은 메시지의 보낸 사람("Client 2", "내 메시지", "돌아옴")이다.
    void Log(Kind kind, const QString& text, const QString& label = {}){
        const QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
        switch(kind){
        case Kind::Sent:
            sent_list->Add_Line(QString("%1   %2").arg(time, text), QColor(125, 196, 255));
            break;
        case Kind::Received:
            received_list->Add_Line(QString("%1   %2: %3").arg(time, label, text), QColor(140, 230, 140));
            break;
        case Kind::Info:
            received_list->Add_Line(QString("%1   %2").arg(time, text), QColor(175, 175, 175));
            break;
        case Kind::Error:
            received_list->Add_Line(QString("%1   오류: %2").arg(time, text), QColor(255, 128, 118), true);
            break;
        }
    }

    int number;
    bool packet_protocol;
    bool world_protocol;
    NameResolver name_of;
    QByteArray recv_buffer;
    quint32 session_id = 0;
    // v8 월드 상태(시야 안의 다른 플레이어 위치, 내 위치)
    QHash<quint32, QPointF> others;
    QPointF self_position;
    float world_size = 0.0f;
    quint32 input_seq = 0;
    int sent_count = 0;
    int received_count = 0;
    qint64 last_received_ms = 0;
    bool showing_quiet = false;
    bool style_initialized = false;
    WorldMap* world_map = nullptr;
    QLabel* world_status = nullptr;
    QLabel* title;
    LogList* sent_list;
    LogList* received_list;
    QTcpSocket* socket;
    QTimer* auto_timer;
};

ClientCard* As_Card(QWidget* widget){
    return static_cast<ClientCard*>(widget);
}

// 접속 수에 맞춰 카드 격자의 열 수를 정한다. 카드가 너무 작아지지 않게 최대 3열로 둔다.
int Grid_Columns(int count){
    if(count <= 1) return 1;
    if(count <= 4) return 2;
    return 3;
}

QPushButton* Make_Button(const QString& text, QWidget* parent, const char* role = nullptr){
    auto* button = new QPushButton(text, parent);
    if(role) button->setProperty("role", role);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}
}

QString Lab_Style_Sheet(){
    // 버튼과 입력칸을 크게, 주요 버튼은 파란색, 켜진 토글은 빨간색으로 보이게 한다.
    return R"(
        QWidget { font-size: 13px; }
        QPushButton {
            min-height: 34px; padding: 2px 16px;
            border: 1px solid #b9c2cc; border-radius: 6px;
            background: #f7f9fb; color: #1d2733;
        }
        QPushButton:hover { background: #e8f0fa; border-color: #8fb0d6; }
        QPushButton:pressed { background: #d6e4f5; }
        QPushButton:disabled { color: #a6adb4; background: #f1f2f3; border-color: #dfe2e5; }
        QPushButton[role="primary"] { background: #2f6fb0; color: white; border-color: #2a619a; font-weight: bold; }
        QPushButton[role="primary"]:hover { background: #3b7fc4; }
        QPushButton[role="primary"]:disabled { background: #a9c1da; color: #eef3f8; border-color: #a9c1da; }
        QPushButton:checked { background: #d9534f; color: white; border-color: #c9433f; font-weight: bold; }
        QLineEdit { min-height: 32px; padding: 0 8px; border: 1px solid #b9c2cc; border-radius: 6px; background: white; }
        QLineEdit:disabled { background: #f1f2f3; color: #a6adb4; }
        QComboBox, QSpinBox { min-height: 32px; }
        QListWidget#versionList { font-size: 14px; }
        QListWidget#versionList::item { padding: 8px 6px; }
        QListWidget { border: 1px solid #3a3f45; border-radius: 6px; background: #1b1d21; color: #f2f2f2; }
        QListWidget::item:selected { background: #2f6fb0; color: white; }
        QListWidget::item:hover { background: #2a2e34; }
        QPlainTextEdit { border: 1px solid #dfe3e8; border-radius: 6px; background: #fbfcfd; }
        QListWidget#logList { font-size: 12px; }
        QLabel#stepTitle { font-size: 15px; font-weight: bold; color: #2f5d8a; }
        QGroupBox { font-weight: bold; border: 1px solid #d5dbe1; border-radius: 8px; margin-top: 12px; padding: 10px 8px 8px 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #2f5d8a; }
    )";
}

ClientPanel::ClientPanel(QWidget* parent) : QWidget(parent){
    setStyleSheet(Lab_Style_Sheet());
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    // 지금 무엇을 해야 하는지 한 줄로 안내한다. 상태(방 시작 전/연결 전/연결 후)에 따라 문구가 바뀐다.
    guide_label = new QLabel(this);
    guide_label->setObjectName("guideLabel");
    guide_label->setWordWrap(true);
    guide_label->setTextFormat(Qt::RichText);
    guide_label->setStyleSheet("QLabel{background:#2f5d8a; color:white; padding:12px 14px; border-radius:8px; font-size:15px;}");
    // 선택한 서버 버전에서 어떤 결과가 정상인지 알려 준다.
    expect_label = new QLabel(this);
    expect_label->setWordWrap(true);
    expect_label->setTextFormat(Qt::RichText);
    expect_label->setStyleSheet("QLabel{background:#eef5ec; color:#24452a; padding:8px 12px; border-radius:6px;}");

    // ③ 연결
    auto* connect_box = new QGroupBox("③ 클라이언트 연결", this);
    auto* connect_row = new QHBoxLayout(connect_box);
    client_count = new QSpinBox(connect_box);
    client_count->setObjectName("clientCount");
    client_count->setRange(1, 30);
    client_count->setValue(4);
    client_count->setSuffix(" 명");
    client_count->setMinimumWidth(80);
    connect_button = Make_Button("클라이언트 연결", connect_box, "primary");
    connect_button->setObjectName("connectButton");
    disconnect_button = Make_Button("모두 연결 끊기", connect_box);
    disconnect_button->setObjectName("disconnectButton");
    connect_row->addWidget(new QLabel("클라이언트 수", connect_box));
    connect_row->addWidget(client_count);
    connect_row->addSpacing(8);
    connect_row->addWidget(connect_button);
    connect_row->addWidget(disconnect_button);
    connect_row->addStretch(1);

    // ④ 보내기: 첫 줄은 직접 보내기, 둘째 줄은 랜덤/자동/이동
    send_box = new QGroupBox("④ 메시지 보내기", this);
    auto* send_layout = new QVBoxLayout(send_box);
    auto* send_row = new QHBoxLayout();
    sender_combo = new QComboBox(send_box);
    sender_combo->setObjectName("senderCombo");
    sender_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    message_edit = new QLineEdit("hello", send_box);
    message_edit->setObjectName("messageEdit");
    message_edit->setPlaceholderText("보낼 메시지를 입력하세요");
    auto* send_button = Make_Button("보내기", send_box, "primary");
    send_button->setObjectName("sendButton");
    send_button->setMinimumWidth(90);
    send_row->addWidget(new QLabel("보내는 쪽", send_box));
    send_row->addWidget(sender_combo);
    send_row->addWidget(message_edit, 1);
    send_row->addWidget(send_button);

    auto* extra_row = new QHBoxLayout();
    random_button = Make_Button("랜덤 메시지 1번", send_box);
    random_button->setObjectName("randomButton");
    random_button->setToolTip("보내는 쪽 클라이언트마다 임의의 메시지를 한 번 보냅니다.");
    auto_button = Make_Button("▶ 랜덤 자동 보내기", send_box);
    auto_button->setObjectName("autoButton");
    auto_button->setCheckable(true);
    auto_button->setToolTip("보내는 쪽 클라이언트마다 1~3초 간격으로 임의의 메시지를 계속 보냅니다. 다시 누르면 멈춥니다.");
    move_button = Make_Button("랜덤 이동", send_box);
    move_button->setToolTip("v8: 보내는 쪽 클라이언트가 임의 방향으로 계속 움직입니다.");
    stop_move_button = Make_Button("■ 이동 정지", send_box);
    extra_row->addWidget(random_button);
    extra_row->addWidget(auto_button);
    extra_row->addSpacing(12);
    extra_row->addWidget(move_button);
    extra_row->addWidget(stop_move_button);
    extra_row->addStretch(1);
    send_layout->addLayout(send_row);
    send_layout->addLayout(extra_row);

    summary_label = new QLabel(this);
    summary_label->setObjectName("summaryLabel");
    summary_label->setTextFormat(Qt::RichText);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // 카드는 창 너비에 맞춰 줄어들고, 넘치면 세로로만 스크롤한다.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    card_container = new QWidget();
    auto* card_area = new QVBoxLayout(card_container);
    card_area->setContentsMargins(0, 0, 0, 0);
    card_grid = new QGridLayout();
    card_grid->setSpacing(10);
    // 연결 전 빈 화면 대신, 여기에 무엇이 나타나는지 알려 준다.
    empty_hint = new QLabel("클라이언트를 연결하면 여기에 클라이언트마다 카드가 하나씩 나타나고,\n"
                            "각 카드에 그 클라이언트가 보내고(→) 받은(←) 내용이 표시됩니다.", card_container);
    empty_hint->setAlignment(Qt::AlignCenter);
    empty_hint->setStyleSheet("QLabel{color:#888; font-size:14px; padding:50px 10px; border:2px dashed #c4c9ce; border-radius:10px;}");
    card_area->addWidget(empty_hint);
    card_area->addLayout(card_grid);
    card_area->addStretch(1);
    scroll->setWidget(card_container);

    layout->addWidget(guide_label);
    layout->addWidget(expect_label);
    layout->addWidget(connect_box);
    layout->addWidget(send_box);
    layout->addWidget(summary_label);
    layout->addWidget(scroll, 1);

    refresh_timer = new QTimer(this);
    connect(refresh_timer, &QTimer::timeout, this, [this]{ Refresh_Cards(); });
    connect(connect_button, &QPushButton::clicked, this, [this]{ Create_Clients(); });
    connect(disconnect_button, &QPushButton::clicked, this, [this]{ Disconnect_Clients(); });
    connect(send_button, &QPushButton::clicked, this, [this]{ Send_Once(); });
    connect(message_edit, &QLineEdit::returnPressed, this, [this]{ Send_Once(); });
    connect(random_button, &QPushButton::clicked, this, [this]{
        for(QWidget* card : Senders()) As_Card(card)->Send_Random();
    });
    connect(auto_button, &QPushButton::toggled, this, [this](bool on){ Set_Auto_Send(on); });
    connect(sender_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]{
        // 자동 보내기 중에 보내는 쪽을 바꾸면 새 대상으로 다시 시작한다.
        if(auto_button->isChecked()) Set_Auto_Send(true);
    });
    connect(move_button, &QPushButton::clicked, this, [this]{
        for(QWidget* card : Senders()) As_Card(card)->Random_Move();
    });
    connect(stop_move_button, &QPushButton::clicked, this, [this]{
        for(QWidget* card : Senders()) As_Card(card)->Send_Move(0.0f, 0.0f);
    });

    Set_Server_Version(0);
    Set_Room_Active(false);
}

bool ClientPanel::Uses_Packet_Protocol() const{
    return server_version >= 7;
}

QString ClientPanel::Expectation() const{
    if(server_version >= 8){
        return "<b>v8에서 정상인 결과</b> — 채팅은 보낸 사람과 <b>시야(주변 3 × 3칸) 안</b>의 클라이언트만 받습니다. "
               "멀리 떨어진 카드가 못 받는 건 정상입니다. 미니맵: 나(주황), 시야 안 플레이어(파랑).";
    }
    if(server_version == 7){
        return "<b>v7에서 정상인 결과</b> — 누가 보내든 <b>모든 클라이언트</b>가 받습니다(방 전체 방송).";
    }
    return QString("<b>v%1에서 정상인 결과</b> — Echo 서버라 <b>보낸 클라이언트만</b> 자기 메시지를 돌려받습니다. "
                   "다른 카드에 아무것도 오지 않는 건 정상입니다.").arg(server_version > 0 ? server_version : 6);
}

void ClientPanel::Update_Guide(){
    QString step;
    if(!room_active){
        step = "<b>① 왼쪽 목록에서 서버 버전을 고르고 ② '실험방 시작'을 누르세요.</b>";
    }else if(cards.isEmpty()){
        step = "<b>③ 클라이언트 수를 정하고 '클라이언트 연결'을 누르세요.</b>";
    }else{
        step = "<b>④ '보내기' 또는 '랜덤 메시지'를 눌러 보세요.</b> 아래 카드에서 누가 무엇을 받았는지 보입니다. "
               "테두리가 주황색인 카드는 5초 넘게 아무것도 받지 못한 클라이언트입니다.";
    }
    guide_label->setText("지금 할 일 &nbsp;→&nbsp; " + step);
}

void ClientPanel::Set_Server_Version(int version){
    server_version = version;
    const bool world = server_version >= 8;
    move_button->setVisible(world);
    stop_move_button->setVisible(world);
    expect_label->setText(server_version > 0 ? Expectation() : QString());
    expect_label->setVisible(server_version > 0);
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
    if(!active) Disconnect_Clients();
    Update_Guide();
}

void ClientPanel::Create_Clients(){
    if(!room_active) return;
    Disconnect_Clients();
    const bool world = server_version >= 8;
    const int count = client_count->value();
    const int columns = Grid_Columns(count);
    for(int column = 0; column < 3; ++column) card_grid->setColumnStretch(column, column < columns ? 1 : 0);
    empty_hint->hide();
    sender_combo->clear();
    sender_combo->addItem("모든 클라이언트");
    // 카드 로그의 "Session N"을 "Client N"으로 바꿔 보여 주기 위한 조회 함수
    auto name_of = [this](quint32 session) -> QString{
        for(QWidget* card : cards){
            if(As_Card(card)->Session_Id() == session) return As_Card(card)->Name();
        }
        return {};
    };
    for(int i = 1; i <= count; ++i){
        auto* card = new ClientCard(i, Uses_Packet_Protocol(), world, name_of, card_container);
        cards.push_back(card);
        card->Apply_Search(search_query, search_only_matches);
        card_grid->addWidget(card, (i - 1) / columns, (i - 1) % columns);
        sender_combo->addItem(QString("Client %1만").arg(i));
    }
    refresh_timer->start(500);
    disconnect_button->setEnabled(true);
    send_box->setEnabled(true);
    Refresh_Cards();
    Update_Guide();
}

void ClientPanel::Disconnect_Clients(){
    refresh_timer->stop();
    auto_button->setChecked(false);
    for(QWidget* card : cards){
        As_Card(card)->Disconnect();
        delete card;   // 격자 레이아웃은 자식 삭제를 스스로 반영한다.
    }
    cards.clear();
    empty_hint->show();
    sender_combo->clear();
    summary_label->clear();
    disconnect_button->setEnabled(false);
    send_box->setEnabled(false);
    Update_Guide();
}

void ClientPanel::Apply_Search(const QString& query, bool only_matches){
    search_query = query;
    search_only_matches = only_matches;
    for(QWidget* card : cards) As_Card(card)->Apply_Search(query, only_matches);
}

QList<QPair<QString, int>> ClientPanel::Search_Matches() const{
    QList<QPair<QString, int>> matches;
    for(QWidget* card : cards) matches.push_back({As_Card(card)->Name(), As_Card(card)->Match_Count()});
    return matches;
}

QList<QWidget*> ClientPanel::Senders() const{
    const int index = sender_combo->currentIndex();
    if(index <= 0) return cards;
    if(index - 1 < cards.size()) return {cards[index - 1]};
    return {};
}

void ClientPanel::Send_Once(){
    for(QWidget* card : Senders()) As_Card(card)->Send(message_edit->text());
}

void ClientPanel::Set_Auto_Send(bool on){
    auto_button->setText(on ? "■ 자동 보내기 멈추기" : "▶ 랜덤 자동 보내기");
    for(QWidget* card : cards) As_Card(card)->Stop_Auto();
    if(!on) return;
    for(QWidget* card : Senders()) As_Card(card)->Start_Auto();
}

void ClientPanel::Refresh_Cards(){
    int connected = 0;
    int sent = 0;
    int received = 0;
    for(QWidget* widget : cards){
        auto* card = As_Card(widget);
        card->Refresh();
        connected += card->Connected() ? 1 : 0;
        sent += card->Sent();
        received += card->Received();
    }
    summary_label->setText(QString("연결 <b>%1 / %2</b> &nbsp;&nbsp;·&nbsp;&nbsp; 전체 보냄 <b>%3</b>"
                                   " &nbsp;&nbsp;·&nbsp;&nbsp; 전체 받음 <b>%4</b>")
        .arg(connected).arg(cards.size()).arg(sent).arg(received));
}
