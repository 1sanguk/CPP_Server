#pragma once
#include <string>
#include <vector>

enum class HostOs { Linux, Windows, MacOs, Unknown };
enum class Availability { Available, UnsupportedOs, NotImplemented };
// Wsl: Windows에서 Linux 전용 서버(v9)를 WSL2 안에서 빌드·실행한다. localhost:9000은 WSL2가 Windows로 넘겨준다.
enum class ExecutionMode { Native, Docker, Wsl, None };

struct ServerDescriptor{
    int version;
    std::string name;
    std::string executable;
    std::string architecture;
    std::string expected_behavior;
    bool implemented;
};

struct ServerEntry{
    ServerDescriptor descriptor;
    Availability availability;
    ExecutionMode execution_mode;
    std::string reason;
};

HostOs Detect_Host_Os();
std::string Host_Os_Name(HostOs os);
std::vector<ServerEntry> Build_Server_Catalog(HostOs os, bool docker_available = false, bool wsl_available = false);
