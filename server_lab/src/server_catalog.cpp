#include <server_catalog.h>

namespace{
bool Supports(int version, HostOs os){
    if(os == HostOs::Linux) return (version >= 1 && version <= 5) || version == 9 || version == 10;
    if(os == HostOs::MacOs) return version >= 1 && version <= 3;
    if(os == HostOs::Windows) return version >= 6 && version <= 8;
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

std::vector<ServerEntry> Build_Server_Catalog(HostOs os, bool docker_available, bool wsl_available){
    const std::vector<ServerDescriptor> descriptors{
        {1,"v1 Blocking","v1_server","single-thread blocking","두 번째 client가 첫 연결 종료까지 대기",true},
        {2,"v2 Thread per connection","v2_server","thread-per-connection","접속 수에 따라 thread 증가",true},
        {3,"v3 Thread pool","v3_server","bounded worker queue","worker 포화 후 queue 대기와 거부",true},
        {4,"v4 epoll reactor","v4_server","single-thread epoll","한 reactor가 여러 socket을 처리",true},
        {5,"v5 epoll + workers","v5_server","reactor and worker pool","reactor 병목과 worker queue 상태 관찰",true},
        {6,"v6 IOCP","v6_server","Windows completion I/O","요청 후 completion worker가 결과 처리",true},
        {7,"v7 IOCP + game logic","v7_server","IOCP workers + single game logic thread",
            "길이 헤더 패킷을 조립해 logic 스레드가 방 전체에 채팅 방송",true},
        {8,"v8 Tick + AOI","v8_server","20Hz tick loop + grid AOI",
            "틱마다 주변 셀 플레이어에게만 이동·입장·퇴장을 묶어 전송",true},
        {9,"v9 Tick + AOI on epoll","v9_server","v8 tick loop + grid AOI on Linux epoll reactor",
            "v8과 같은 화면. 네트워크 계층만 epoll(Linux)로 바뀜",true},
        {10,"v10 Tick + AOI on io_uring","v10_server","v8 tick loop + grid AOI on Linux io_uring completions",
            "v8·v9와 같은 화면. 네트워크 계층만 io_uring(Linux 완료 통지)으로 바뀜",true},
    };
    std::vector<ServerEntry> result;
    for(const auto& descriptor : descriptors){
        if(!descriptor.implemented){
            result.push_back({descriptor, Availability::NotImplemented, ExecutionMode::None, "아직 구현되지 않은 버전입니다."});
        }else if(os == HostOs::MacOs && descriptor.version >= 4 && descriptor.version <= 5 && docker_available){
            result.push_back({descriptor, Availability::Available, ExecutionMode::Docker, "사용 가능 — Docker Linux"});
        }else if(os == HostOs::Windows && descriptor.version >= 9 && wsl_available){
            result.push_back({descriptor, Availability::Available, ExecutionMode::Wsl,
                descriptor.version == 10 ? "사용 가능 — WSL2 (처음 시작할 때 WSL 안에서 빌드, liburing-dev 필요)"
                                         : "사용 가능 — WSL2 (처음 시작할 때 WSL 안에서 빌드)"});
        }else if(!Supports(descriptor.version, os)){
            result.push_back({descriptor, Availability::UnsupportedOs, ExecutionMode::None,
                descriptor.version >= 9 ? "Linux 서버입니다. Windows에서는 WSL2가 있어야 실행할 수 있습니다."
                : descriptor.version <= 5 ? "POSIX/Linux 계열 서버로 현재 OS에서 실행할 수 없습니다."
                                          : "Windows IOCP 전용 서버입니다."});
        }else{
            result.push_back({descriptor, Availability::Available, ExecutionMode::Native, "사용 가능 — Native"});
        }
    }
    return result;
}
