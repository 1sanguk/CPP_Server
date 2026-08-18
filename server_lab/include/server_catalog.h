#pragma once
#include <string>
#include <vector>

enum class HostOs { Linux, Windows, MacOs, Unknown };
enum class Availability { Available, UnsupportedOs, NotImplemented };
enum class ExecutionMode { Native, Docker, None };

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
std::vector<ServerEntry> Build_Server_Catalog(HostOs os, bool docker_available = false);
