#include <thread_pool_echo_server.h>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <thread>

int main(void){
    // SIGINT/SIGTERM을 모든 이후 생성 스레드에서 막고 main 스레드가 sigwait()로 동기 처리한다.
    // signal handler 안에서 mutex나 condition_variable을 만지는 비동기 안전성 문제를 피한다.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    ThreadPoolEchoServer tpes{9000};
    std::thread server_thread(&ThreadPoolEchoServer::Server_Run, &tpes);

    int received_signal = 0;
    sigwait(&stop_signals, &received_signal);
    std::cout << "Stop signal received: " << received_signal << '\n';

    tpes.Stop();
    server_thread.join();

    return 0;
}
