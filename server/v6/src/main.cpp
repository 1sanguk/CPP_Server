#include <iostream>
#include <thread>

#include <iocp_echo_server.h>

int main(void){
    IocpEchoServer ies{9000};
    std::thread server_thread(&IocpEchoServer::Server_Run, &ies);

    std::cout << "[main] Press Enter to stop the server." << std::endl;
    std::cin.get();

    ies.Stop();
    if(server_thread.joinable()){
        server_thread.join();
    }

    return 0;
}
