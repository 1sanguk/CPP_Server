#include <server_catalog.h>
#include <iostream>

// assert는 Release 구성에서 NDEBUG로 사라진다. start.ps1이 이 테스트를 Release로 실행하므로
// 실패 횟수를 직접 세어 종료 코드로 돌려준다.
namespace{
int failures = 0;

void Check(bool condition, const char* expression, int line){
    if(!condition){
        ++failures;
        std::cerr << "FAILED line " << line << ": " << expression << std::endl;
    }
}
}
#define CHECK(expr) Check((expr), #expr, __LINE__)

int main(){
    auto linux_catalog = Build_Server_Catalog(HostOs::Linux, false);
    CHECK(linux_catalog.size() == 7);
    for(int i=0; i<5; ++i) CHECK(linux_catalog[i].availability == Availability::Available);
    CHECK(linux_catalog[5].availability == Availability::UnsupportedOs);
    CHECK(linux_catalog[6].availability == Availability::UnsupportedOs);

    auto windows_catalog = Build_Server_Catalog(HostOs::Windows, false);
    for(int i=0; i<5; ++i) CHECK(windows_catalog[i].availability == Availability::UnsupportedOs);
    CHECK(windows_catalog[5].availability == Availability::Available);
    CHECK(windows_catalog[6].availability == Availability::Available);

    auto mac_catalog = Build_Server_Catalog(HostOs::MacOs, false);
    for(int i=0; i<3; ++i) CHECK(mac_catalog[i].availability == Availability::Available);
    for(int i=3; i<7; ++i) CHECK(mac_catalog[i].availability != Availability::Available);

    auto mac_docker_catalog = Build_Server_Catalog(HostOs::MacOs, true);
    CHECK(mac_docker_catalog[3].execution_mode == ExecutionMode::Docker);
    CHECK(mac_docker_catalog[4].execution_mode == ExecutionMode::Docker);
    CHECK(mac_docker_catalog[5].availability == Availability::UnsupportedOs);

    if(failures != 0){
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "server catalog tests passed" << std::endl;
    return 0;
}
