#pragma once

#include <packet.h>

#include <cstddef>
#include <optional>
#include <vector>

// TCP 바이트 스트림을 패킷 단위로 잘라내는 조립기.
// 한 번의 Recv에 패킷 여러 개가 들어오거나, 헤더/본문이 여러 Recv에 나뉘어 들어와도
// Append()로 계속 누적하고 Next()로 완성된 패킷만 꺼낸다.
//
// 스레드 안전하지 않다. v7~v8은 세션마다 WSARecv를 하나만 pending으로 두므로 같은 세션의
// 조립기는 한 번에 하나의 completion worker만 사용한다.
class PacketAssembler{
    private:
        std::vector<char> buffer;
        // 이미 꺼낸 패킷의 바이트는 바로 지우지 않고 read_offset만 전진시킨다.
        // 매 패킷마다 vector 앞부분을 erase하면 남은 데이터를 계속 앞으로 복사해야 하기 때문이다.
        std::size_t read_offset = 0;
        bool error = false;

        void Compact();

    public:
        void Append(const char* data, std::size_t length);
        // 완성된 패킷이 있으면 하나를 반환하고, 아직 부족하거나 오류 상태면 nullopt를 반환한다.
        std::optional<protocol::Packet> Next();
        // size 필드가 헤더보다 작거나 최대 패킷 크기를 넘으면 오류 상태가 된다.
        // 이후 스트림의 경계를 신뢰할 수 없으므로 호출자는 연결을 끊어야 한다.
        bool Has_Error() const;
        std::size_t Buffered_Bytes() const;
};
