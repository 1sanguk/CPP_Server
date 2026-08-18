#include <main_window.h>
#include <client_panel.h>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
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
}

MainWindow::MainWindow() : catalog(Build_Server_Catalog(Detect_Host_Os(), Docker_Available())){
    setWindowTitle("MMO Server Architecture Lab");
    resize(1450, 720);
    auto* root = new QWidget(this);
    auto* layout = new QHBoxLayout(root);
    auto* splitter = new QSplitter(Qt::Horizontal, root);
    version_list = new QListWidget(root);
    auto* server_panel = new QWidget(root);
    auto* right = new QVBoxLayout();
    status_label = new QLabel(QString("Host OS: %1").arg(QString::fromStdString(Host_Os_Name(Detect_Host_Os()))), root);
    detail_label = new QLabel(root);
    detail_label->setWordWrap(true);
    log_view = new QPlainTextEdit(root);
    log_view->setReadOnly(true);
    start_button = new QPushButton("이 버전으로 실험방 시작", root);
    for(const auto& entry : catalog){
        auto* item = new QListWidgetItem(QString::fromStdString(entry.descriptor.name), version_list);
        if(entry.availability != Availability::Available){
            item->setForeground(Qt::gray);
            item->setToolTip(QString::fromStdString(entry.reason));
        }
    }
    right->addWidget(status_label);
    right->addWidget(detail_label);
    right->addWidget(start_button);
    right->addWidget(log_view, 1);
    server_panel->setLayout(right);
    client_panel = new ClientPanel(root);
    splitter->addWidget(version_list);
    splitter->addWidget(server_panel);
    splitter->addWidget(client_panel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 3);
    layout->addWidget(splitter);
    setCentralWidget(root);
    connect(version_list, &QListWidget::currentRowChanged, this, [this]{ Refresh_Detail(); });
    connect(start_button, &QPushButton::clicked, this, [this]{
        if(room_process) Stop_Room();
        else Start_Room();
    });
    version_list->setCurrentRow(0);
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
        log_view->appendPlainText(QString::fromLocal8Bit(process->readAll()));
    });
    Track_Room_Process(process);
    process->start();
    log_view->appendPlainText(QString("v%1 실험방 시작: %2")
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
            log_view->appendPlainText(QString("서버 실행 실패: %1").arg(process->errorString()));
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
            log_view->appendPlainText(QString("서버 종료 코드: %1").arg(code));
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
    log_view->appendPlainText("실험방 종료를 요청했습니다.");

    QProcess* process = room_process;
    if(!active_container_name.isEmpty()){
        auto* stop = new QProcess(this);
        stop->setProgram("docker");
        stop->setArguments({"stop", "-t", "2", active_container_name});
        stop->setProcessChannelMode(QProcess::MergedChannels);
        connect(stop, &QProcess::readyRead, this, [this, stop]{
            if(closing) return;
            log_view->appendPlainText(QString::fromLocal8Bit(stop->readAll()));
        });
        connect(stop, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), stop, &QObject::deleteLater);
        stop->start();
    }else{
        process->terminate();
    }

    QTimer::singleShot(3000, process, [process]{
        if(process->state() != QProcess::NotRunning) process->kill();
    });
}

void MainWindow::Build_And_Start(const ServerDescriptor& descriptor){
    start_button->setEnabled(false);
    log_view->appendPlainText(QString("v%1 실행 파일이 없어 자동 빌드를 시작합니다...").arg(descriptor.version));

    auto* configure = new QProcess(this);
    configure->setProgram("cmake");
    configure->setArguments({"-S", Repo_Root() + "/server", "-B", Repo_Root() + "/build/server"});
    configure->setProcessChannelMode(QProcess::MergedChannels);
    connect(configure, &QProcess::readyRead, this, [this, configure]{
        if(closing) return;
        log_view->appendPlainText(QString::fromLocal8Bit(configure->readAll()));
    });
    connect(configure, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, configure, descriptor](int code, QProcess::ExitStatus status){
            if(closing) return;
            configure->deleteLater();
            if(status != QProcess::NormalExit || code != 0){
                log_view->appendPlainText("CMake 설정에 실패했습니다.");
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
                log_view->appendPlainText(QString::fromLocal8Bit(build->readAll()));
            });
            connect(build, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, build, descriptor](int build_code, QProcess::ExitStatus build_status){
                    if(closing) return;
                    build->deleteLater();
                    Refresh_Detail();
                    if(build_status != QProcess::NormalExit || build_code != 0){
                        log_view->appendPlainText("선택한 서버 버전의 빌드에 실패했습니다.");
                        return;
                    }
                    const QString executable = Find_Executable(descriptor);
                    if(executable.isEmpty()){
                        log_view->appendPlainText("빌드는 성공했지만 실행 파일을 찾지 못했습니다.");
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
            log_view->appendPlainText(QString::fromLocal8Bit(process->readAll()));
        });
        active_container_name = container_name;
        Track_Room_Process(process);
        process->start();
        log_view->appendPlainText(QString("Docker 실험방 시작: v%1, localhost:9000").arg(entry.descriptor.version));
        return;
    }
    QString executable = Find_Executable(entry.descriptor);
    if(executable.isEmpty()){
        Build_And_Start(entry.descriptor);
        return;
    }
    Launch_Native(entry.descriptor, executable);
}
