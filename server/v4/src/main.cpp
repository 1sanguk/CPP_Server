#include <epoll_echo_server.h>
#include <csignal>
#include <thread>
#include <iostream>
#include <pthread.h>

int main(void){
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    EpollEchoServer ees{9000};
    std::thread server_thread(&EpollEchoServer::Server_Run, &ees);

    int received_signal = 0;
    sigwait(&stop_signals, &received_signal);
    std::cout << "[main] Stop signal received: " << received_signal << std::endl;

    ees.Stop();
    server_thread.join();

    return 0;
}
