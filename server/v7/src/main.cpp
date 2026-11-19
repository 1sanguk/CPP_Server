#include <iostream>
#include <thread>

#include <iocp_game_server.h>

int main(void){
    IocpGameServer server{9000};
    std::thread server_thread(&IocpGameServer::Server_Run, &server);

    std::cout << "[main] Set V7_LOG_LEVEL=debug before launch to see per-recv/send activity." << std::endl;
    std::cout << "[main] Press Enter to stop the server." << std::endl;
    std::cin.get();

    server.Stop();
    if(server_thread.joinable()){
        server_thread.join();
    }

    return 0;
}
