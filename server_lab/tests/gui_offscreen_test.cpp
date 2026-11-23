// 화면에 창을 띄우지 않는(offscreen) GUI 테스트.
//
// 실제 서버(v6~v8)를 백그라운드로 띄우고, ClientPanel을 offscreen으로 만든 뒤 버튼을 코드로 눌러
// "클라이언트 4개 연결 → Client 1만 메시지 보내기"를 실행한다. 그 결과 각 카드가 받은 수가
// 버전별 기대(v6: 보낸 쪽만, v7: 전원, v8: 같은 시야 안 전원)와 맞는지 확인하고, 화면을 PNG로 저장한다.
// 마우스나 키보드를 실제로 움직이지 않으므로 사용자가 다른 작업 중이어도 돌릴 수 있다.
//
// 사용법: server_lab_gui_test [스크린샷 저장 폴더]

#include <client_panel.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>

namespace{
int failures = 0;

void Check(bool condition, const QString& message){
    std::printf("%s %s\n", condition ? "[PASS]" : "[FAIL]", message.toUtf8().constData());
    if(!condition) ++failures;
}

// 이벤트 루프를 돌리면서 조건이 참이 될 때까지 기다린다(소켓 신호와 타이머가 처리되도록).
bool Wait_Until(const std::function<bool()>& condition, int timeout_ms){
    QElapsedTimer timer;
    timer.start();
    while(timer.elapsed() < timeout_ms){
        if(condition()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return condition();
}

void Pump(int ms){
    Wait_Until([]{ return false; }, ms);
}

bool Port_Open(){
    QTcpSocket probe;
    probe.connectToHost("127.0.0.1", 9000);
    const bool ok = probe.waitForConnected(200);
    probe.abort();
    return ok;
}

QString Server_Path(int version){
    return QDir::cleanPath(QStringLiteral(SERVER_LAB_REPO_ROOT)) +
        QString("/build/server/v%1/Release/v%1_server.exe").arg(version);
}

QList<QFrame*> Cards(ClientPanel& panel){
    QList<QFrame*> cards;
    for(QFrame* frame : panel.findChildren<QFrame*>("clientCard")) cards.push_back(frame);
    // findChildren 순서는 생성 순서와 같지만, 번호 순서를 확실히 하려고 제목으로 정렬하지 않고 그대로 쓴다.
    return cards;
}

template <typename T>
T* Child(ClientPanel& panel, const char* name){
    T* child = panel.findChild<T*>(name);
    if(!child) std::printf("[FAIL] widget not found: %s\n", name);
    return child;
}

void Save(ClientPanel& panel, const QString& dir, const QString& name){
    Pump(300);   // 레이아웃이 자리 잡은 뒤에 찍는다.
    const QString path = dir + "/" + name;
    panel.grab().save(path);
    std::printf("       screenshot: %s\n", QDir::toNativeSeparators(path).toUtf8().constData());
}

// 한 버전을 처음부터 끝까지 돌린다. expected(i)는 Client 1이 한 번 보냈을 때 카드 i가 받아야 하는 수다.
void Run_Version(int version, const QStringList& server_env, const std::function<int(int)>& expected,
    const QString& shot_dir){
    std::printf("\n== v%d ==\n", version);
    if(Port_Open()){
        Check(false, "port 9000 is already in use (another server is running)");
        return;
    }
    QProcess server;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for(const QString& pair : server_env) env.insert(pair.section('=', 0, 0), pair.section('=', 1));
    server.setProcessEnvironment(env);
    server.setProcessChannelMode(QProcess::MergedChannels);
    if(version == 9){
        // v9는 Linux 서버라 Server Lab처럼 WSL2로 띄운다. 환경변수는 WSL로 넘어가지 않으므로 env 명령으로 준다.
        // exec로 bash를 서버로 바꿔, 아래에서 보내는 줄바꿈이 서버의 표준 입력에 닿게 한다.
        server.start("wsl.exe", {"-e", "bash", "-lc", "exec env " + server_env.join(' ') + " ~/v9build/v9/v9_server"});
    }else{
        server.start(Server_Path(version), {});
    }
    if(!server.waitForStarted(3000)){
        Check(false, QString("start server v%1").arg(version));
        return;
    }
    Check(Wait_Until(Port_Open, version == 9 ? 20000 : 5000), "server listens on 9000");

    ClientPanel panel;
    panel.resize(1100, 760);
    panel.show();   // offscreen이라 실제 화면에는 나타나지 않는다.
    panel.Set_Server_Version(version);
    panel.Set_Room_Active(true);
    Save(panel, shot_dir, QString("v%1_1_room_started.png").arg(version));

    auto* count = Child<QSpinBox>(panel, "clientCount");
    auto* connect_button = Child<QPushButton>(panel, "connectButton");
    auto* sender = Child<QComboBox>(panel, "senderCombo");
    auto* message = Child<QLineEdit>(panel, "messageEdit");
    auto* send = Child<QPushButton>(panel, "sendButton");
    auto* guide = Child<QLabel>(panel, "guideLabel");
    if(count && connect_button && sender && message && send && guide){
        Check(guide->text().contains("③"), "guide shows step 3 after room start");
        count->setValue(4);
        connect_button->click();
        const bool all_connected = Wait_Until([&]{
            const auto cards = Cards(panel);
            if(cards.size() != 4) return false;
            for(QFrame* card : cards) if(!card->property("connected").toBool()) return false;
            return true;
        }, 5000);
        Check(all_connected, "4 client cards connected");
        Check(guide->text().contains("④"), "guide shows step 4 after connecting");
        // v7은 입장 알림(S_Notice)이 먼저 오고, v8은 월드 입장/시야 패킷이 먼저 온다. 받은 수(채팅)에는 안 들어간다.
        Pump(500);

        sender->setCurrentIndex(1);   // "Client 1만"
        message->setText("hello from 1");
        send->click();
        const auto cards = Cards(panel);
        Wait_Until([&]{
            for(int i = 0; i < cards.size(); ++i){
                if(cards[i]->property("received").toInt() < expected(i)) return false;
            }
            return true;
        }, 3000);
        Pump(500);   // 기대보다 더 받는 경우도 잡으려고 조금 더 기다린다.
        for(int i = 0; i < cards.size(); ++i){
            const int got = cards[i]->property("received").toInt();
            Check(got == expected(i), QString("Client %1 received %2 (expected %3)").arg(i + 1).arg(got).arg(expected(i)));
        }
        Check(cards.value(0) && cards[0]->property("sent").toInt() == 1, "Client 1 sent 1");
        if(version >= 8){
            const int nearby = cards.value(0) ? cards[0]->property("nearby").toInt() : -1;
            Check(nearby == 3, QString("v8 Client 1 sees %1 others (expected 3, one cell)").arg(nearby));
        }
        Save(panel, shot_dir, QString("v%1_2_after_send.png").arg(version));

        // 검색: Client 1은 보낸 칸 1줄 + (돌려받았으면) 받은 칸 1줄, 나머지는 받은 칸 줄 수만큼 찾아야 한다.
        panel.Apply_Search("hello from 1", true);
        int search_total = 0;
        for(const auto& [name, matches] : panel.Search_Matches()) search_total += matches;
        for(int i = 0; i < cards.size(); ++i){
            const int want = (i == 0 ? 1 : 0) + expected(i);
            const int got = cards[i]->property("matches").toInt();
            Check(got == want, QString("search 'hello from 1': Client %1 matched %2 lines (expected %3)").arg(i + 1).arg(got).arg(want));
        }
        Save(panel, shot_dir, QString("v%1_2b_search.png").arg(version));
        panel.Apply_Search({}, false);
        Check(search_total > 0, QString("search found %1 lines in total").arg(search_total));

        // 🎲 랜덤 메시지 1번: "모든 클라이언트"가 한 번씩 보낸다.
        auto* random = Child<QPushButton>(panel, "randomButton");
        auto* auto_send = Child<QPushButton>(panel, "autoButton");
        if(random && auto_send){
            sender->setCurrentIndex(0);
            random->click();
            Pump(300);
            int sent_total = 0;
            for(QFrame* card : cards) sent_total += card->property("sent").toInt();
            Check(sent_total == 1 + 4, QString("random button: every client sent once (total sent %1)").arg(sent_total));

            // ▶ 랜덤 자동 보내기: 1~3초 간격이므로 3.5초 안에 모두 한 번 이상 더 보내야 한다.
            QList<int> before;
            for(QFrame* card : cards) before.push_back(card->property("sent").toInt());
            auto_send->click();
            Check(auto_send->isChecked(), "auto send toggled on");
            Pump(3500);
            bool all_sent_more = true;
            for(int i = 0; i < cards.size(); ++i){
                all_sent_more = all_sent_more && cards[i]->property("sent").toInt() > before[i];
            }
            Check(all_sent_more, "auto send: every client kept sending");
            Save(panel, shot_dir, QString("v%1_3_auto_send.png").arg(version));
            auto_send->click();
            Check(!auto_send->isChecked(), "auto send toggled off");
            QList<int> stopped;
            for(QFrame* card : cards) stopped.push_back(card->property("sent").toInt());
            Pump(3200);
            bool none_sent = true;
            for(int i = 0; i < cards.size(); ++i) none_sent = none_sent && cards[i]->property("sent").toInt() == stopped[i];
            Check(none_sent, "auto send stopped");
        }

        // v8 랜덤 이동(돌아다니기): 작은 월드에서 몇 초 돌아다니면 반드시 벽에 닿는다.
        // 벽에 닿은 뒤에도 방향을 바꿔 계속 움직이는지, 이동 정지 후에는 멈추는지 확인한다.
        auto* move = panel.findChild<QPushButton*>("moveButton");
        auto* stop_move = panel.findChild<QPushButton*>("stopMoveButton");
        if(version >= 8 && move && stop_move){
            auto positions = [&]{
                QList<QPointF> points;
                for(QFrame* card : cards) points.push_back({card->property("posX").toDouble(), card->property("posY").toDouble()});
                return points;
            };
            auto all_moved = [](const QList<QPointF>& a, const QList<QPointF>& b){
                for(int i = 0; i < a.size(); ++i){
                    if(QLineF(a[i], b[i]).length() < 1.0) return false;
                }
                return true;
            };
            sender->setCurrentIndex(0);
            move->click();
            Pump(5000);   // 월드 120, 속도 60이면 2초 안에 벽에 닿는다
            const QList<QPointF> after_walls = positions();
            Pump(1500);
            Check(all_moved(after_walls, positions()), "random move keeps moving after hitting the wall");
            Save(panel, shot_dir, QString("v%1_4_wander.png").arg(version));
            stop_move->click();
            Pump(600);
            const QList<QPointF> stopped_at = positions();
            Pump(1200);
            bool none_moved = true;
            const QList<QPointF> later = positions();
            for(int i = 0; i < later.size(); ++i) none_moved = none_moved && QLineF(stopped_at[i], later[i]).length() < 0.5;
            Check(none_moved, "stop move stops wandering");
        }
    }

    panel.Shutdown();
    Pump(200);
    server.write("\n");   // v6~v9 서버는 Enter로 정상 종료한다(v9는 WSL을 거쳐 표준 입력으로 받는다).
    server.closeWriteChannel();
    if(!server.waitForFinished(5000)){
        server.kill();
        server.waitForFinished(2000);
        Check(false, "server stopped by Enter");
    }else{
        Check(server.exitCode() == 0, QString("server exit code %1").arg(server.exitCode()));
    }
}
}

int main(int argc, char** argv){
    // 화면에 창을 띄우지 않는다. 플러그인 경로는 ctest가 QT_PLUGIN_PATH로 넘겨 준다.
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    // offscreen 플랫폼은 시스템 글꼴을 스스로 찾지 않아 글자가 네모로 찍힌다. Windows 글꼴 폴더를 알려 준다.
    if(qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")){
        const QString windows_dir = qEnvironmentVariable("WINDIR", "C:/Windows");
        qputenv("QT_QPA_FONTDIR", QDir(windows_dir).filePath("Fonts").toLocal8Bit());
    }
    QApplication app(argc, argv);
    // 실제 Windows 화면과 비슷하게 보이도록 한글 기본 글꼴을 쓴다.
    app.setFont(QFont("Malgun Gothic", 9));
    const QString shot_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath();
    QDir().mkpath(shot_dir);

    // v6 Echo: 보낸 Client 1만 자기 메시지를 돌려받는다.
    Run_Version(6, {}, [](int i){ return i == 0 ? 1 : 0; }, shot_dir);
    // v7 방송: 4명 모두 받는다.
    Run_Version(7, {}, [](int){ return 1; }, shot_dir);
    // v8: 셀 하나(셀 크기 = 월드 크기)로 두면 모두 같은 시야라 전원이 받는다.
    Run_Version(8, {"V8_CELL_SIZE=1000", "V8_WORLD_SIZE=120"}, [](int){ return 1; }, shot_dir);
    // v9: v8과 같은 프로토콜·규칙이라 같은 기대값. WSL2에 ~/v9build/v9/v9_server가 빌드돼 있을 때만 돌린다.
    QProcess probe;
    probe.start("wsl.exe", {"-e", "bash", "-lc", "test -x ~/v9build/v9/v9_server"});
    if(probe.waitForFinished(20000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0){
        Run_Version(9, {"V9_CELL_SIZE=1000", "V9_WORLD_SIZE=120"}, [](int){ return 1; }, shot_dir);
    }else{
        std::printf("\n== v9 == skipped (WSL2 or ~/v9build/v9/v9_server not found)\n");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
