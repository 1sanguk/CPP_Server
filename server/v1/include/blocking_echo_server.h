#pragma once
#include <cstddef>

class BlockingEchoServer{
    private:
        int server_port;  // 이 서버가 바인딩할 포트 번호
        int fd = -1;      // listen 소켓의 파일 디스크립터. -1이면 아직 socket()이 호출되지 않은 상태(센티널 값)

        // send()가 요청한 길이보다 적게 전송하는 partial send에 대비해,
        // 남은 범위를 반복 전송하고 전체 성공 여부를 호출부에 반환한다.
        bool Send_All(
            int client_fd,
            const char* data,
            std::size_t length
        );

    public:
        // port만 저장. 실제 소켓 생성/bind/listen은 Server_Run()에서 수행.
        BlockingEchoServer(const int port);

        // fd가 유효하면(-1이 아니면) close()로 소켓을 정리.
        ~BlockingEchoServer();
        
        // socket()~listen()으로 서버를 준비시키고, accept 루프를 돌며 클라이언트를 순차적으로 처리.
        void Server_Run();
};
