#include <server_catalog.h>

namespace{
bool Supports(int version, HostOs os){
    if(os == HostOs::Linux) return version >= 1 && version <= 5;
    if(os == HostOs::MacOs) return version >= 1 && version <= 3;
    if(os == HostOs::Windows) return version == 6 || version == 7;
    return false;
}
}

HostOs Detect_Host_Os(){
#if defined(_WIN32)
    return HostOs::Windows;
#elif defined(__linux__)
    return HostOs::Linux;
#elif defined(__APPLE__)
    return HostOs::MacOs;
#else
    return HostOs::Unknown;
#endif
}

std::string Host_Os_Name(HostOs os){
    switch(os){
        case HostOs::Linux: return "Linux";
        case HostOs::Windows: return "Windows";
        case HostOs::MacOs: return "macOS";
        default: return "Unknown";
    }
}

std::vector<ServerEntry> Build_Server_Catalog(HostOs os, bool docker_available){
    const std::vector<ServerDescriptor> descriptors{
        {1,"v1 Blocking","v1_server","single-thread blocking","두 번째 client가 첫 연결 종료까지 대기",true},
        {2,"v2 Thread per connection","v2_server","thread-per-connection","접속 수에 따라 thread 증가",true},
        {3,"v3 Thread pool","v3_server","bounded worker queue","worker 포화 후 queue 대기와 거부",true},
        {4,"v4 epoll reactor","v4_server","single-thread epoll","한 reactor가 여러 socket을 처리",true},
        {5,"v5 epoll + workers","v5_server","reactor and worker pool","reactor 병목과 worker queue 상태 관찰",true},
        {6,"v6 IOCP","v6_server","Windows completion I/O","요청 후 completion worker가 결과 처리",true},
        {7,"v7 IOCP + game logic","v7_server","IOCP and logic queue","network와 game logic queue 분리",false},
    };
    std::vector<ServerEntry> result;
    for(const auto& descriptor : descriptors){
        if(!descriptor.implemented){
            result.push_back({descriptor, Availability::NotImplemented, ExecutionMode::None, "아직 구현되지 않은 버전입니다."});
        }else if(os == HostOs::MacOs && descriptor.version >= 4 && descriptor.version <= 5 && docker_available){
            result.push_back({descriptor, Availability::Available, ExecutionMode::Docker, "사용 가능 — Docker Linux"});
        }else if(!Supports(descriptor.version, os)){
            result.push_back({descriptor, Availability::UnsupportedOs, ExecutionMode::None,
                descriptor.version <= 5 ? "POSIX/Linux 계열 서버로 현재 OS에서 실행할 수 없습니다."
                                        : "Windows IOCP 전용 서버입니다."});
        }else{
            result.push_back({descriptor, Availability::Available, ExecutionMode::Native, "사용 가능 — Native"});
        }
    }
    return result;
}
