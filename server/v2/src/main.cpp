#include <thread_connection_server.h>

int main(void){
    
    ThreadPerConnectionEchoServer tpces{9000};

    tpces.Server_Run();

    return 0;
}