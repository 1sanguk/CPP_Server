#include <iocp_echo_server.h>

IocpEchoServer::IocpEchoServer(int port){
    this->server_port = port;
}

IocpEchoServer::~IocpEchoServer(){
    this->Proceed_Stop();
}

bool IocpEchoServer::Init_Winsock(){
    bool flag = false;

    if(this->winsock_init_state){
        return true;
    }

    WSADATA wsaData;
    int WSAStartup_result = WSAStartup(MAKEWORD(2,2, &wsaData));
    if(WSAStartup_result != 0){
        WSAGetLastError("WSA Startup Failed", WSAStartup_result);
    }
    else{
        flag = true;
        this->winsock_init_state = true;
    }

    return flag;
}

bool IocpEchoServer::Create_Listen_Socket(){
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(this->server_port);
    addr.flag = WSA_FLAG_OVERLAPPED;

    int bind_result = bind();



}


bool IocpEchoServer::Create_Completion_Port(){

}

void IocpEchoServer::Start_Completion_Workers(){

}

void IocpEchoServer::Accept_Loop(){

}

bool IocpEchoServer::Register_Client(SOCKET client_socket){

}

void IocpEchoServer::Completion_worker_Loop(){

}

void IocpEchoServer::Close_Client(SOCKET client_socket){

}

void IocpEchoServer::Clean_Up(){

}

void IocpEchoServer::Server_Run(){

}

void IocpEchoServer::Stop(){

}

void IocpEchoServer::Proceed_Stop(){
    this->Stop();
    this->Clean_Up();

    return;
}