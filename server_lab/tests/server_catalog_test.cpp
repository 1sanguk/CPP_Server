#include <server_catalog.h>
#include <cassert>

int main(){
    auto linux_catalog = Build_Server_Catalog(HostOs::Linux, false);
    for(int i=0; i<5; ++i) assert(linux_catalog[i].availability == Availability::Available);
    assert(linux_catalog[5].availability == Availability::UnsupportedOs);
    assert(linux_catalog[6].availability == Availability::NotImplemented);

    auto windows_catalog = Build_Server_Catalog(HostOs::Windows, false);
    for(int i=0; i<5; ++i) assert(windows_catalog[i].availability == Availability::UnsupportedOs);
    assert(windows_catalog[5].availability == Availability::Available);
    assert(windows_catalog[6].availability == Availability::NotImplemented);

    auto mac_catalog = Build_Server_Catalog(HostOs::MacOs, false);
    for(int i=0; i<3; ++i) assert(mac_catalog[i].availability == Availability::Available);
    for(int i=3; i<7; ++i) assert(mac_catalog[i].availability != Availability::Available);

    auto mac_docker_catalog = Build_Server_Catalog(HostOs::MacOs, true);
    assert(mac_docker_catalog[3].execution_mode == ExecutionMode::Docker);
    assert(mac_docker_catalog[4].execution_mode == ExecutionMode::Docker);
    assert(mac_docker_catalog[5].availability == Availability::UnsupportedOs);
}
