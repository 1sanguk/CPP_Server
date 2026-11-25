#include <main_window.h>
#include <client_panel.h>
#include <log_list.h>
#include <QCheckBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace{
bool Docker_Available(){
    QProcess process;
    process.start("docker", {"info"});
    return process.waitForFinished(5000) && process.exitCode() == 0;
}

// Windows에서 WSL2 배포판이 있어 명령을 실행할 수 있는지. 꺼져 있던 WSL을 켜는 데 몇 초 걸릴 수 있다.
bool Wsl_Available(){
    if(Detect_Host_Os() != HostOs::Windows) return false;
    QProcess process;
    process.start("wsl.exe", {"-e", "true"});
    return process.waitForFinished(15000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// D:/Projects/x → /mnt/d/Projects/x (WSL에서 본 Windows 경로)
QString To_Wsl_Path(const QString& windows_path){
    QString path = QDir::fromNativeSeparators(windows_path);
    if(path.size() >= 2 && path[1] == ':'){
        path = "/mnt/" + path.left(1).toLower() + path.mid(2);
    }
    return path;
}
}

MainWindow::MainWindow() : catalog(Build_Server_Catalog(Detect_Host_Os(), Docker_Available(), Wsl_Available())){
    setWindowTitle("MMO Server Architecture Lab");
    resize(1700, 940);
    setStyleSheet(Lab_Style_Sheet());
    // 화면 구성
    //   왼쪽(좁게): ① 서버 버전 고르기, ② 실험방 시작
    //   오른쪽 맨 위: 검색줄(Ctrl+F) — 모든 클라이언트 카드와 서버 로그를 한꺼번에 찾는다
    //   오른쪽 위: 클라이언트 테스트(③ 연결, ④ 보내기, 카드)
    //   오른쪽 아래: 서버 로그(항상 보이고, 경계를 끌어 높이를 조절)
    auto* root = new QWidget(this);
    auto* layout = new QHBoxLayout(root);

    auto* left = new QWidget(root);
    left->setFixedWidth(270);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    auto* version_title = new QLabel("① 서버 버전 고르기", left);
    version_title->setObjectName("stepTitle");
    version_list = new QListWidget(left);
    version_list->setObjectName("versionList");
    for(const auto& entry : catalog){
        auto* item = new QListWidgetItem(QString::fromStdString(entry.descriptor.name), version_list);
        if(entry.availability != Availability::Available){
            item->setForeground(Qt::gray);
            item->setToolTip(QString::fromStdString(entry.reason));
        }
    }
    auto* server_title = new QLabel("② 실험방(서버) 실행", left);
    server_title->setObjectName("stepTitle");
    detail_label = new QLabel(left);
    detail_label->setWordWrap(true);
    detail_label->setStyleSheet("QLabel{color:#444; background:white; border:1px solid #dfe3e8; border-radius:6px; padding:8px;}");
    start_button = new QPushButton("이 버전으로 실험방 시작", left);
    start_button->setProperty("role", "primary");
    start_button->setMinimumHeight(44);
    start_button->setCursor(Qt::PointingHandCursor);
    status_label = new QLabel(QString("Host OS: %1").arg(QString::fromStdString(Host_Os_Name(Detect_Host_Os()))), left);
    status_label->setStyleSheet("QLabel{color:#888;}");
    left_layout->addWidget(version_title);
    // 목록은 버전 수만큼만 높이를 주고, 바로 아래에 ② 실험방 시작이 오게 한다(남는 공간은 맨 아래로).
    version_list->setFixedHeight(version_list->count() * 38 + 8);
    left_layout->addWidget(version_list);
    left_layout->addSpacing(8);
    left_layout->addWidget(server_title);
    left_layout->addWidget(detail_label);
    left_layout->addWidget(start_button);
    left_layout->addStretch(1);
    left_layout->addWidget(status_label);

    auto* right = new QWidget(root);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);

    // 검색줄
    auto* search_row = new QHBoxLayout();
    search_edit = new QLineEdit(right);
    search_edit->setPlaceholderText("검색 (Ctrl+F) — 모든 클라이언트의 보낸/받은 메시지와 서버 로그에서 찾습니다");
    search_edit->setClearButtonEnabled(true);
    search_only = new QCheckBox("찾은 줄만 보기", right);
    search_summary = new QLabel(right);
    search_summary->setTextFormat(Qt::RichText);
    search_summary->setStyleSheet("QLabel{color:#555;}");
    search_row->addWidget(new QLabel("<b>검색</b>", right));
    search_row->addWidget(search_edit, 2);
    search_row->addWidget(search_only);
    search_row->addWidget(search_summary, 3);

    // 클라이언트 테스트(가운데)와 서버 로그(오른쪽 세로 칸)를 좌우로 나눈다. 경계를 끌어 폭을 조절한다.
    auto* columns = new QSplitter(Qt::Horizontal, right);
    client_panel = new ClientPanel(columns);
    auto* server_log_panel = new QWidget(columns);
    auto* server_log_layout = new QVBoxLayout(server_log_panel);
    server_log_layout->setContentsMargins(4, 0, 0, 0);
    auto* server_log_title = new QLabel("서버 로그", server_log_panel);
    server_log_title->setObjectName("stepTitle");
    auto* server_log_note = new QLabel("실행 중인 서버 프로그램이 출력하는 내용입니다. "
                                       "왼쪽 경계를 끌어 폭을 조절하고, 줄을 골라 Ctrl+C로 복사할 수 있습니다.", server_log_panel);
    server_log_note->setWordWrap(true);
    server_log_note->setStyleSheet("QLabel{color:#888;}");
    log_view = new LogList(5000, server_log_panel);
    log_view->setStyleSheet("QListWidget{font-family:Consolas, 'Malgun Gothic'; font-size:12px;}");
    server_log_layout->addWidget(server_log_title);
    server_log_layout->addWidget(server_log_note);
    server_log_layout->addWidget(log_view, 1);
    server_log_panel->setMinimumWidth(260);
    columns->addWidget(client_panel);
    columns->addWidget(server_log_panel);
    columns->setStretchFactor(0, 3);
    columns->setStretchFactor(1, 1);
    columns->setChildrenCollapsible(false);
    columns->setSizes({960, 440});
    right_layout->addLayout(search_row);
    right_layout->addWidget(columns, 1);

    layout->addWidget(left);
    layout->addWidget(right, 1);
    setCentralWidget(root);

    connect(version_list, &QListWidget::currentRowChanged, this, [this]{ Refresh_Detail(); });
    connect(start_button, &QPushButton::clicked, this, [this]{
        if(room_process) Stop_Room();
        else Start_Room();
    });
    connect(search_edit, &QLineEdit::textChanged, this, [this]{ Apply_Search(); });
    connect(search_only, &QCheckBox::toggled, this, [this]{ Apply_Search(); });
    // Ctrl+F: 검색칸으로 이동, Esc: 검색 지우기
    auto* find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this]{
        search_edit->setFocus();
        search_edit->selectAll();
    });
    auto* clear = new QShortcut(QKeySequence(Qt::Key_Escape), search_edit);
    clear->setContext(Qt::WidgetShortcut);
    connect(clear, &QShortcut::activated, search_edit, &QLineEdit::clear);
    // 검색 중에 새 로그가 들어오면 건수가 바뀌므로 주기적으로 다시 센다.
    auto* summary_timer = new QTimer(this);
    connect(summary_timer, &QTimer::timeout, this, [this]{ Update_Search_Summary(); });
    summary_timer->start(500);
    version_list->setCurrentRow(0);
}

void MainWindow::Apply_Search(){
    const QString query = search_edit->text();
    const bool only = search_only->isChecked();
    client_panel->Apply_Search(query, only);
    log_view->Apply_Search(query, only);
    Update_Search_Summary();
}

void MainWindow::Update_Search_Summary(){
    if(search_edit->text().isEmpty()){
        search_summary->clear();
        return;
    }
    // "총 12건 — Client 1: 3 · Client 2: 4 · 서버 로그: 5" (0건인 곳은 생략)
    int total = 0;
    QStringList parts;
    for(const auto& [name, count] : client_panel->Search_Matches()){
        total += count;
        if(count > 0) parts.push_back(QString("%1: %2").arg(name).arg(count));
    }
    const int server_count = log_view->Match_Count();
    total += server_count;
    if(server_count > 0) parts.push_back(QString("서버 로그: %1").arg(server_count));
    search_summary->setText(total == 0
        ? QString("<span style='color:#c9433f'>찾은 줄 없음</span>")
        : QString("<b>%1건</b> — %2").arg(total).arg(parts.join(" · ")));
}

// 서버 출력은 줄 중간에서 잘려 올 수 있어서, 줄바꿈이 올 때까지 모았다가 한 줄씩 넣는다.
void MainWindow::Append_Server_Log(const QString& text){
    server_log_partial += text;
    server_log_partial.remove('\r');
    qsizetype newline = 0;
    while((newline = server_log_partial.indexOf('\n')) >= 0){
        const QString line = server_log_partial.left(newline);
        server_log_partial.remove(0, newline + 1);
        if(line.trimmed().isEmpty()) continue;
        const QString lower = line.toLower();
        const bool error = lower.contains("error") || lower.contains("failed") || line.contains("실패");
        log_view->Add_Line(line, error ? QColor(255, 128, 118) : QColor(240, 240, 240), error);
    }
}

// Lab 자체가 남기는 안내(실험방 시작/종료, 빌드 등). 서버 출력과 구분되게 파란 굵은 글씨로 쓴다.
void MainWindow::Add_Lab_Message(const QString& text){
    Flush_Server_Log();
    log_view->Add_Line("── " + text, QColor(125, 196, 255), true);
}

void MainWindow::Flush_Server_Log(){
    if(!server_log_partial.trimmed().isEmpty()) log_view->Add_Line(server_log_partial.trimmed());
    server_log_partial.clear();
}

void MainWindow::closeEvent(QCloseEvent* event){
    closing = true;
    client_panel->Shutdown();

    if(!active_container_name.isEmpty()){
        QProcess docker_stop;
        docker_stop.start("docker", {"stop", "-t", "1", active_container_name});
        docker_stop.waitForFinished(3000);
    }

    const auto child_processes = findChildren<QProcess*>();
    for(QProcess* process : child_processes){
        disconnect(process, nullptr, this, nullptr);
        if(process->state() == QProcess::NotRunning) continue;
        process->terminate();
        if(!process->waitForFinished(500)){
            process->kill();
            process->waitForFinished(1000);
        }
    }
    room_process = nullptr;
    active_container_name.clear();
    event->accept();
}

void MainWindow::Refresh_Detail(){
    int row = version_list->currentRow();
    if(row < 0) return;
    const auto& entry = catalog[static_cast<std::size_t>(row)];
    // 버전을 고르는 순간 클라이언트 화면의 "기대 결과" 문구도 그 버전 기준으로 바꾼다(방 실행 중이면 그대로 둔다).
    if(!room_process) client_panel->Set_Server_Version(entry.descriptor.version);
    detail_label->setText(QString("구조: %1\n예상 현상: %2\n상태: %3")
        .arg(QString::fromStdString(entry.descriptor.architecture),
             QString::fromStdString(entry.descriptor.expected_behavior),
             QString::fromStdString(entry.reason)));
    if(!room_process){
        start_button->setText("이 버전으로 실험방 시작");
        start_button->setEnabled(entry.availability == Availability::Available);
    }
}

QString MainWindow::Find_Executable(const ServerDescriptor& descriptor) const{
    const QString name = QString::fromStdString(descriptor.executable);
    const QString suffix = Detect_Host_Os() == HostOs::Windows ? ".exe" : "";
    const QString cwd = QDir::currentPath();
    const QString repo = Repo_Root();
    const QStringList candidates{
        repo + "/build/server/v" + QString::number(descriptor.version) + "/" + name + suffix,
        repo + "/build/server/v" + QString::number(descriptor.version) + "/Debug/" + name + suffix,
        repo + "/build/server/v" + QString::number(descriptor.version) + "/Release/" + name + suffix,
        cwd + "/build/server/v" + QString::number(descriptor.version) + "/" + name + suffix,
        cwd + "/build/server/v" + QString::number(descriptor.version) + "/Debug/" + name + suffix,
        cwd + "/build/server/v" + QString::number(descriptor.version) + "/Release/" + name + suffix,
        cwd + "/server/build/v" + QString::number(descriptor.version) + "/" + name + suffix,
        cwd + "/server/build/v" + QString::number(descriptor.version) + "/Debug/" + name + suffix,
        cwd + "/server/build/v" + QString::number(descriptor.version) + "/Release/" + name + suffix,
        cwd + "/server/build_linux/v" + QString::number(descriptor.version) + "/" + name + suffix,
        QCoreApplication::applicationDirPath() + "/" + name + suffix,
    };
    for(const auto& path : candidates) if(QFileInfo::exists(path)) return path;
    return {};
}

QString MainWindow::Repo_Root() const{
    return QDir::cleanPath(QStringLiteral(SERVER_LAB_REPO_ROOT));
}

void MainWindow::Launch_Native(const ServerDescriptor& descriptor, const QString& executable){
    auto* process = new QProcess(this);
    process->setProgram(executable);
    process->setWorkingDirectory(Repo_Root());
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyRead, this, [this, process]{
        if(closing) return;
        Append_Server_Log(QString::fromLocal8Bit(process->readAll()));
    });
    Track_Room_Process(process);
    process->start();
    Add_Lab_Message(QString("v%1 실험방 시작: %2")
        .arg(descriptor.version).arg(executable));
}

void MainWindow::Track_Room_Process(QProcess* process){
    room_process = process;
    connect(process, &QProcess::started, this, [this, process]{
        if(closing) return;
        if(room_process != process) return;
        start_button->setText("방 종료");
        start_button->setEnabled(true);
        client_panel->Set_Room_Active(true);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error){
        if(closing) return;
        if(error == QProcess::FailedToStart){
            Add_Lab_Message(QString("서버 실행 실패: %1").arg(process->errorString()));
            if(room_process == process){
                room_process = nullptr;
                active_container_name.clear();
                client_panel->Set_Room_Active(false);
                Refresh_Detail();
            }
            process->deleteLater();
        }
    });
    connect(process, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, process](int code, QProcess::ExitStatus){
            if(closing) return;
            Add_Lab_Message(QString("서버 종료 코드: %1").arg(code));
            if(room_process == process){
                room_process = nullptr;
                active_container_name.clear();
                client_panel->Set_Room_Active(false);
                Refresh_Detail();
            }
            process->deleteLater();
        });
}

void MainWindow::Stop_Room(){
    if(!room_process) return;
    start_button->setText("종료 중...");
    start_button->setEnabled(false);
    Add_Lab_Message("실험방 종료를 요청했습니다.");

    QProcess* process = room_process;
    if(!active_container_name.isEmpty()){
        auto* stop = new QProcess(this);
        stop->setProgram("docker");
        stop->setArguments({"stop", "-t", "2", active_container_name});
        stop->setProcessChannelMode(QProcess::MergedChannels);
        connect(stop, &QProcess::readyRead, this, [this, stop]{
            if(closing) return;
            Append_Server_Log(QString::fromLocal8Bit(stop->readAll()));
        });
        connect(stop, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), stop, &QObject::deleteLater);
        stop->start();
    }else if(Detect_Host_Os() == HostOs::Windows){
        // v6~v8 콘솔 서버는 Enter 입력으로 graceful shutdown을 시작한다. Windows의 terminate()는
        // 콘솔 프로그램에 전달되지 않아 3초 뒤 강제 종료(kill)로 끝나므로 표준 입력으로 줄바꿈을 보낸다.
        process->write("\n");
        process->closeWriteChannel();
    }else{
        process->terminate();
    }

    QTimer::singleShot(3000, process, [process]{
        if(process->state() != QProcess::NotRunning) process->kill();
    });
}

void MainWindow::Build_And_Start(const ServerDescriptor& descriptor){
    start_button->setEnabled(false);
    Add_Lab_Message(QString("v%1 실행 파일이 없어 자동 빌드를 시작합니다...").arg(descriptor.version));

    auto* configure = new QProcess(this);
    configure->setProgram("cmake");
    configure->setArguments({"-S", Repo_Root() + "/server", "-B", Repo_Root() + "/build/server"});
    configure->setProcessChannelMode(QProcess::MergedChannels);
    connect(configure, &QProcess::readyRead, this, [this, configure]{
        if(closing) return;
        Append_Server_Log(QString::fromLocal8Bit(configure->readAll()));
    });
    connect(configure, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, configure, descriptor](int code, QProcess::ExitStatus status){
            if(closing) return;
            configure->deleteLater();
            if(status != QProcess::NormalExit || code != 0){
                Add_Lab_Message("CMake 설정에 실패했습니다.");
                Refresh_Detail();
                return;
            }

            auto* build = new QProcess(this);
            build->setProgram("cmake");
            build->setArguments({"--build", Repo_Root() + "/build/server", "--target",
                QString::fromStdString(descriptor.executable), "--parallel"});
            build->setProcessChannelMode(QProcess::MergedChannels);
            connect(build, &QProcess::readyRead, this, [this, build]{
                if(closing) return;
                Append_Server_Log(QString::fromLocal8Bit(build->readAll()));
            });
            connect(build, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, build, descriptor](int build_code, QProcess::ExitStatus build_status){
                    if(closing) return;
                    build->deleteLater();
                    Refresh_Detail();
                    if(build_status != QProcess::NormalExit || build_code != 0){
                        Add_Lab_Message("선택한 서버 버전의 빌드에 실패했습니다.");
                        return;
                    }
                    const QString executable = Find_Executable(descriptor);
                    if(executable.isEmpty()){
                        Add_Lab_Message("빌드는 성공했지만 실행 파일을 찾지 못했습니다.");
                        return;
                    }
                    Launch_Native(descriptor, executable);
                });
            build->start();
        });
    configure->start();
}

void MainWindow::Start_Room(){
    int row = version_list->currentRow();
    if(row < 0) return;
    const auto& entry = catalog[static_cast<std::size_t>(row)];
    client_panel->Set_Server_Version(entry.descriptor.version);
    if(entry.execution_mode == ExecutionMode::Docker){
        auto* process = new QProcess(this);
        const QString container_name = QString("cpp-server-lab-v%1-%2")
            .arg(entry.descriptor.version).arg(QCoreApplication::applicationPid());
        process->setProgram("docker");
        process->setArguments({"run", "--rm", "--name", container_name, "-p", "9000:9000",
            "cpp-server-lab:latest",
            QString("/opt/server/v%1/%2").arg(entry.descriptor.version)
                .arg(QString::fromStdString(entry.descriptor.executable))});
        process->setProcessChannelMode(QProcess::MergedChannels);
        connect(process, &QProcess::readyRead, this, [this, process]{
            if(closing) return;
            Append_Server_Log(QString::fromLocal8Bit(process->readAll()));
        });
        active_container_name = container_name;
        Track_Room_Process(process);
        process->start();
        Add_Lab_Message(QString("Docker 실험방 시작: v%1, localhost:9000").arg(entry.descriptor.version));
        return;
    }
    if(entry.execution_mode == ExecutionMode::Wsl){
        // WSL 안에서 빌드(바뀐 것만)한 뒤 서버를 실행한다. exec로 bash를 서버로 바꿔, 실험방 종료 때 Windows에서 보내는
        // 줄바꿈이 서버의 표준 입력에 바로 닿게 한다(v9·v10은 SIGTERM 외에 Enter로도 정상 종료한다).
        // 빌드 폴더는 WSL 홈에 둔다. /tmp는 WSL이 꺼지면 지워진다. v10은 WSL에 liburing-dev가 있어야 빌드된다.
        const QString server_dir = To_Wsl_Path(Repo_Root() + "/server");
        const QString name = QString::fromStdString(entry.descriptor.executable);
        const QString script = QString(
            "cd '%1' && cmake -S . -B ~/v9build -DCMAKE_BUILD_TYPE=Release > /dev/null && "
            "cmake --build ~/v9build --target %2 -j > /dev/null && exec ~/v9build/v%3/%2")
            .arg(server_dir, name, QString::number(entry.descriptor.version));
        auto* process = new QProcess(this);
        process->setProgram("wsl.exe");
        process->setArguments({"-e", "bash", "-lc", script});
        process->setProcessChannelMode(QProcess::MergedChannels);
        connect(process, &QProcess::readyRead, this, [this, process]{
            if(closing) return;
            Append_Server_Log(QString::fromUtf8(process->readAll()));
        });
        Track_Room_Process(process);
        process->start();
        Add_Lab_Message(QString("WSL2 실험방 시작: v%1, localhost:9000 (처음에는 빌드 때문에 시간이 걸립니다)")
            .arg(entry.descriptor.version));
        return;
    }
    QString executable = Find_Executable(entry.descriptor);
    if(executable.isEmpty()){
        Build_And_Start(entry.descriptor);
        return;
    }
    Launch_Native(entry.descriptor, executable);
}
