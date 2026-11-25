#include <uring_world_server.h>

#include <csignal>
#include <cstdio>
#include <iostream>
#include <pthread.h>
#include <thread>
#include <unistd.h>

int main(void){
    // SIGINT/SIGTERM은 모든 스레드에서 막고 main 스레드가 sigwait로 받는다(v5와 같다).
    // 시그널 핸들러 안에서는 mutex나 로그를 쓸 수 없지만, sigwait는 평범한 함수 안이라 Stop()을 바로 부를 수 있다.
    // 서버 스레드를 만들기 전에 막아야 새 스레드들이 막힌 마스크를 물려받는다.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    UringWorldServer server{9000};
    std::thread server_thread(&UringWorldServer::Server_Run, &server);

    // v6~v8처럼 Enter(줄바꿈)로도 끝낼 수 있게 한다. Windows의 Server Lab이 WSL을 통해 띄운 v9를 끌 때 표준 입력으로
    // 줄바꿈을 보낸다. 받으면 자기 자신에게 SIGTERM을 보내 아래 sigwait 경로 하나로 종료한다.
    // 표준 입력이 바로 EOF인 경우(백그라운드 실행, /dev/null)에는 아무것도 하지 않는다. 읽기에서 막힌 채 남을 수 있어 detach한다.
    std::thread([]{
        int c = 0;
        while((c = std::getchar()) != EOF){
            if(c == '\n'){
                kill(getpid(), SIGTERM);
                return;
            }
        }
    }).detach();

    std::cout << "[main] Set V10_LOG_LEVEL=debug to see per-session activity. Enter, Ctrl+C or SIGTERM stops the server." << std::endl;
    int received_signal = 0;
    sigwait(&stop_signals, &received_signal);
    std::cout << "[main] stop signal received: " << received_signal << std::endl;

    server.Stop();
    server_thread.join();
    return 0;
}
