#include <iostream>
#include <thread>

#include <iocp_world_server.h>

int main(void){
    IocpWorldServer server{9000};
    std::thread server_thread(&IocpWorldServer::Server_Run, &server);

    std::cout << "[main] Set V8_LOG_LEVEL=debug before launch to see per-recv/send activity." << std::endl;
    std::cout << "[main] Press Enter to stop the server." << std::endl;
    std::cin.get();

    server.Stop();
    if(server_thread.joinable()){
        server_thread.join();
    }

    return 0;
}
