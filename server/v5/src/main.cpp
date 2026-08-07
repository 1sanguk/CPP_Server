#include <epoll_thread_pool_server.h>
#include <csignal>
#include <thread>
#include <iostream>
#include <pthread.h>

int main(void){
    // SIGINT/SIGTERM을 main thread가 sigwait()로 동기 처리해 서버 Stop() 경로를 보장한다.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    EpollThreadPoolServer etps{9000};
    std::thread server_thread(&EpollThreadPoolServer::Server_Run, &etps);

    int received_signal = 0;
    sigwait(&stop_signals, &received_signal);
    std::cout << "[main] Stop signal received: " << received_signal << std::endl;

    etps.Stop();
    server_thread.join();

    return 0;
}