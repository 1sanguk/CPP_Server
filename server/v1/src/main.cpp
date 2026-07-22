#include <blocking_echo_server.h>

int main(void){
    BlockingEchoServer bes = BlockingEchoServer(9000);

    bes.Server_Run();

    return 0;
}