#pragma once

class ThreadPerConnectionEchoServer{
    private:
        int server_port;  // 이 서버가 바인딩할 포트 번호
        int fd = -1;      // listen 소켓의 파일 디스크립터. -1이면 아직 socket()이 호출되지 않은 상태(센티널 값)

        // accept()로 받은 client_fd 하나를 전담하는 함수. 스레드마다 하나씩 이 함수가 돌아감.
        // recv/send 루프를 돌다가 연결이 끊기면 이 fd를 닫고 스레드가 종료된다.
        void Process_Client(int client_fd);

    public:
        // port만 저장. 실제 소켓 생성/bind/listen은 Server_Run()에서 수행.
        ThreadPerConnectionEchoServer(const int port);

        // fd가 유효하면(-1이 아니면) close()로 listen 소켓을 정리.
        // 클라이언트별 fd는 각자의 Process_Client 스레드 안에서 닫히므로 여기서 다루지 않는다.
        ~ThreadPerConnectionEchoServer();

        // socket()~listen()으로 서버를 준비시키고, accept 루프를 돌며
        // 접속마다 새 스레드를 Process_Client에 넘기고 detach한다(thread-per-connection).
        void Server_Run();
};