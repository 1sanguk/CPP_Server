# 버그 재현 / 트러블슈팅 기록

단순히 "안전하게 짜서 안 터졌다"가 아니라, 실제로 문제를 만들어보고 왜 안전한지 이해했다는 걸 남기기 위한 기록. 아래 두 종류를 모두 남긴다.

1. **의도적으로 만든 버그**: 학습 목적으로 안전장치를 일부러 빼고 문제를 재현한 뒤 고친 기록 (예: v2에서 mutex 없이 공유 리스트 접근 → ThreadSanitizer로 race condition 확인 → mutex 추가 후 재검증)
2. **실제로 겪은 버그**: 구현 중 우연히 만난 문제 (deadlock, 커넥션 누수, use-after-free 등)와 원인 분석, 해결 과정

각 기록은 아래 형식으로 남긴다:

```
### [버전] 문제 제목
- 증상: 무엇이 어떻게 잘못됐는지
- 원인: 근본 원인
- 재현 방법: 어떻게 재현했는지 (도구, 조건)
- 해결: 어떻게 고쳤는지
- 배운 점: (선택)
```

---

### [v2] 
#### 멤버 함수 포인터 스코프 누락으로 컴파일 실패
- 증상: `thread t(Process_Client, this, accept_result);`처럼 클래스 스코프 없이 멤버 함수 이름만 넘기면 컴파일 에러가 발생함.
- 원인: 비정적 멤버 함수의 주소를 얻으려면 반드시 `&ClassName::MemberFunc` 형태로 명시적으로 스코프를 붙여야 함. 이름만 쓰면 컴파일러가 어떤 클래스의 멤버인지 알 수 없어 포인터-투-멤버 타입을 만들지 못함.
- 재현 방법: 위 코드를 그대로 작성 후 빌드해서 에러 확인.
- 해결: `thread t(&ThreadPerConnectionEchoServer::Process_Client, this, accept_result);`로 수정.
- 배운 점: 멤버 함수를 콜백/스레드 진입점으로 넘길 때는 항상 `&Class::Member` 형태를 써야 한다.

---

### [v3] 
#### listen 소켓 shutdown 후에도 accept가 끝나지 않음

- 증상: 활성 연결 6개 상태에서 `Stop()`을 호출하면 client의 blocking `recv()`는 모두
  해제됐지만 `server_thread.join()`이 30초 이상 끝나지 않았다.
- 원인: macOS에서는 다른 스레드가 listen fd에 `shutdown(SHUT_RDWR)`을 호출하는 것만으로
  blocking `accept()`가 반드시 해제되지 않았다.
- 재현 방법: worker 4개보다 많은 6개 연결을 유지하고 별도 스레드에서 `Stop()`을 호출한 뒤
  server thread를 join했다.
- 해결: listen fd를 `O_NONBLOCK`으로 설정하고 `poll()`에 100ms timeout을 적용해 종료
  상태를 주기적으로 확인했다. SIGINT/SIGTERM은 main의 `sigwait()`에서 받아 `Stop()`을
  호출하도록 구성했다.
- 배운 점: blocking 시스템 호출의 중단 동작은 플랫폼 차이가 있으므로 종료 설계를 특정
  `shutdown()` 동작 하나에만 의존하면 안 된다.

#### non-blocking listen 설정 후 client recv가 EAGAIN으로 종료됨

- 증상: 연결 직후 worker의 `recv()`가 `Resource temporarily unavailable`을 출력하고
  client 연결을 닫았다.
- 원인: 로컬 환경에서 listen fd의 `O_NONBLOCK` 상태가 accept된 client fd에도 이어졌지만,
  v3의 `Process_Client()`는 blocking 소켓을 전제로 `EAGAIN`을 처리하지 않았다.
- 재현 방법: non-blocking listen fd로 서버를 실행하고 장기 연결 6개를 생성했다.
- 해결: accept 직후 client fd의 `O_NONBLOCK` 플래그를 제거해 v3의 blocking worker
  구조를 유지했다.
- 배운 점: listener와 accepted socket의 플래그 상태를 플랫폼에서 당연히 분리해줄 것이라고
  가정하지 말고, 서버가 원하는 client socket 모드를 명시적으로 설정해야 한다.

---

### [v4] 
#### macOS 에디터에서 epoll 관련 빨간줄 표시

- 증상: `sys/epoll.h`, `epoll_event`, `epoll_ctl`, `epoll_wait`, `EPOLLIN` 등이 에디터에서
  오류처럼 표시됐다.
- 원인: v4는 Linux 전용 API인 epoll을 사용하지만, macOS 에디터의 IntelliSense는 macOS SDK
  기준으로 분석해 Linux 헤더를 찾지 못했다.
- 재현 방법: macOS에서 v4 소스를 열어 `sys/epoll.h` 포함부와 epoll API 사용부를 확인했다.
- 해결: 실제 기준 빌드는 Docker Ubuntu 24.04 컨테이너로 잡고, macOS 에디터 빨간줄은
  Linux 전용 버전의 분석 한계로 분리했다.
- 배운 점: 소스 분석 환경과 실제 빌드/배포 타겟이 다르면 IDE 표시와 실제 컴파일 결과가
  다를 수 있다.

#### epoll_wait 결과 배열 크기 오류

- 증상: `epoll_wait()`에 `kMaxEvents = 64`를 넘겼지만 두 번째 인자로 `epoll_event` 하나의
  주소만 넘겨 컴파일러가 buffer overflow 경고를 출력했다.
- 원인: 등록용 `epoll_event` 하나와 `epoll_wait()` 결과를 받을 이벤트 배열의 역할을
  구분하지 않았다.
- 재현 방법: `epoll_event epollevent{}` 하나를 만들고 `epoll_wait(epoll_fd, &epollevent,
  kMaxEvents, ...)` 형태로 빌드했다.
- 해결: listen fd 등록용 `listen_event`와 결과 수신용 `wait_events[kMaxEvents]`를 분리했다.
- 배운 점: `epoll_ctl()`은 이벤트 하나를 등록하지만, `epoll_wait()`는 준비된 이벤트 여러 개를
  배열에 써준다.

#### accept/recv를 하지 않아 같은 fd 이벤트가 반복됨

- 증상: 새 클라이언트 접속 후 `Wait_Events[0]: 3` 로그가 무한히 출력됐고, client fd 등록 후에는
  `Wait_Events[0]: 5` 로그가 반복됐다.
- 원인: level-triggered epoll에서는 준비 상태가 남아 있으면 같은 이벤트를 계속 돌려준다.
  listen fd 이벤트는 `accept()`로 접속 큐를 소비해야 하고, client fd 이벤트는 `recv()`로
  소켓 버퍼를 소비해야 한다.
- 재현 방법: listen fd를 epoll에 등록한 뒤 `accept()` 없이 로그만 출력하거나, client fd를
  등록한 뒤 `recv()` 없이 로그만 출력했다.
- 해결: listen fd 이벤트에서는 `accept()`를 호출하고, client fd 이벤트에서는 `recv()`로
  데이터를 읽도록 분기했다.
- 배운 점: epoll은 이벤트를 “처리”해주지 않고 준비된 fd만 알려준다. 준비 상태를 소비하는
  시스템 호출은 서버 코드가 직접 해야 한다.

#### Docker 컨테이너 서버에 macOS nc가 접속하지 못함

- 증상: 컨테이너 안에서 v4 서버가 실행 중인데 macOS 터미널에서 `nc 127.0.0.1 9000` 접속이
  되지 않았다.
- 원인: 컨테이너의 `127.0.0.1`과 macOS host의 `127.0.0.1`은 다른 네트워크 네임스페이스다.
- 재현 방법: `docker run --rm -it -v "$PWD":/workspace cpp-server-dev ./.../v4_server`로
  서버를 실행한 뒤 host 터미널에서 `nc`로 접속했다.
- 해결: 서버 실행 시 `-p 9000:9000` 옵션을 추가해 host 9000번 포트를 컨테이너 9000번 포트에
  매핑했다.
- 배운 점: Docker에서 외부 host 접속 테스트를 하려면 포트 매핑이 필요하다.

---

### [v5] 
#### `Worker_Loop`의 wait predicate 람다가 지역 변수를 캡처하지 않아 컴파일 실패

- 증상: `Worker_Loop(int index)`의 `job_condition.wait()` predicate 람다 안에서
  `this->workers[index]...`를 썼는데 `'index' is not captured`, `expected
  unqualified-id before '.' token` 에러가 났다.
- 원인: 람다 캡처 목록이 `[this]`뿐이었다. `this`를 통한 멤버 접근은 되지만, 함수의
  지역 변수(파라미터 `index` 포함)는 캡처 목록에 따로 적지 않으면 람다 안에서 쓸 수 없다.
  같은 자리에 `workers[index]..empty()`처럼 `.job_queue`가 빠진 오타도 섞여 있었다.
- 재현 방법: `[this]`로만 캡처한 람다 안에서 `index`를 참조하는 코드를 그대로 빌드했다.
- 해결: 캡처 목록을 `[this, index]`로 고치고, `workers[index].job_queue.empty()`로
  오타를 바로잡았다.
- 배운 점: 람다는 캡처 목록에 명시한 것만 쓸 수 있다. `this`를 캡처했다고 그 함수의
  다른 지역 변수까지 자동으로 보이는 게 아니다.

#### 여러 스레드가 동시에 로그를 찍어 `Sending to client` 로그의 fd 값이 깨짐

- 증상: 300개 동시 접속 부하 테스트 중 로그를 보니 `Sending to client
  6281472909701472: ...`처럼 실제로 존재할 수 없는 fd 값이 찍혔다.
- 원인: reactor, 워커, monitor 세 종류의 스레드가 동기화 없이 동시에 `std::cout`에
  출력하고 있었다. 한 줄의 로그가 `cout << a; cout << b; cout << endl;`처럼 여러 번의
  출력 호출로 나뉘어 있어서, 그 사이사이에 다른 스레드의 출력이 끼어들며 바이트 단위로
  섞였다.
- 재현 방법: 2000개 클라이언트가 각각 20개 메시지를 보내는 부하(총 40,000 echo)를
  걸고 로그에서 비정상적으로 긴 fd 값을 검색했다.
- 해결: `log_mutex`로 보호되는 `Logging()` 함수 하나로 모든 출력을 통일했다. 호출부는
  `std::ostringstream`으로 한 줄 전체를 문자열로 완성한 뒤 `Logging()`을 한 번만
  호출하도록 바꿔, 한 줄의 출력이 여러 스레드로 쪼개지지 않게 했다.
- 해결 확인: 로그가 깨졌던 것과 같은 2000개 동시 접속 부하(40000/40000 echo 성공)를
  다시 걸어 로그에서 비정상적으로 긴 fd 값이 더 이상 나오지 않는 것을 확인했다.
- 배운 점: 뮤텍스로 감싸더라도 한 줄의 로그를 여러 번의 출력 호출로 나누면 그 사이사이는
  여전히 보호되지 않는다. 로그 한 줄은 완성된 문자열을 만든 뒤 한 번에 출력해야 한다.

#### 부하 테스트 중 재현된 연결 끊김이 서버 버그가 아니라 테스트 하네스 문제였음

- 증상: 300개 클라이언트를 30초 이상에 걸쳐 천천히 늘리는 지속 부하 테스트에서, 요청
  3000개 중 1개가 매번 "서버가 응답을 다 보내기 전에 연결을 끊음"으로 실패했다
  (`recv()`가 마지막 메시지 도중 0을 반환).
- 원인 조사: 연결마다 고유 ID를 붙이고 `Delete_Client_Fd`/`Process_Job` 등 주요 지점에
  로그를 추가해 재현해보니, 실패한 연결은 3번째 메시지까지는 정상 처리됐지만 마지막
  메시지가 `send_buffers`에 쌓인 채 `EPOLLOUT`으로 다 보내지기 전에 연결이 끊겼다.
  서버 프로세스를 감싼 테스트용 `timeout` 값(45초)이 실제 테스트 소요 시간(약 44.5초)과
  거의 같아서, 테스트가 끝나기 직전 `timeout`이 서버에 `SIGTERM`을 보내 `Clean_Up()`이
  아직 못 보낸 데이터가 남은 연결을 강제로 닫아버린 것이었다.
- 재현 방법: 서버를 감싼 `timeout` 값을 테스트 소요 시간과 거의 같게 맞추고 동일한
  지속 부하를 반복 실행했다.
- 해결: 서버를 감싼 `timeout` 값을 충분히 크게(120초) 늘려서 같은 부하를 다시
  실행하니 3000/3000 전부 성공했다 — 서버 로직 자체의 버그가 아니었다.
- 배운 점: 부하 테스트에서 나온 실패가 항상 서버 코드의 버그는 아니다. 재현 조건(이
  경우 테스트 하네스의 타임아웃 여유)을 하나씩 바꿔가며 원인이 서버 쪽인지 테스트
  도구 쪽인지 좁혀야 한다. 다만 이 조사로 `Clean_Up()`이 종료 시 아직 못 보낸 세션
  버퍼 데이터를 flush하지 않고 바로 닫아버린다는, 실제로 존재하는 동작을 확인했다.
- 후속 조치 (2026-08-07): `Clean_Up()`이 각 client fd를 닫기 전에 남은 `send_buffers`
  데이터를 `Send_All()`로 한 번 더 flush 시도하도록 고쳤다. 버그를 처음 재현했던 것과
  같은 조건(서버 `timeout` 45초, 300 clients 지속 부하)으로 다시 테스트하니 3000/3000
  전부 성공했다 (feedback.md에 `[완료]`로 갱신).

---

### [v6]

#### 정상 `AcceptEx` 완료를 0바이트 연결 종료로 처리함

- 증상: client가 연결되면 Accept 후속 처리에 들어가지 못하고 context가 닫힐 수 있었다.
- 원인: I/O 타입을 확인하기 전에 `byte_transferred == 0`을 모든 completion의 연결 종료로
  처리했다. 초기 수신 길이가 0인 `AcceptEx()`의 정상 완료도 0바이트다.
- 해결: 실패 completion을 먼저 처리하고, `Recv` 타입이면서 0바이트인 경우에만 정상 연결
  종료로 분기했다. `Accept`의 0바이트 completion은 IOCP 등록과 최초 Recv 게시로 이어진다.
- 확인: 단일 반복 echo 20/20과 다중 client echo 1000/1000 성공을 확인했다.

#### partial send 후 다음 Recv의 버퍼 시작 주소를 복구하지 않음

- 증상: partial send가 발생해 `WSABUF.buf`가 앞으로 이동한 뒤 다음 Recv가 버퍼 중간에서
  전체 크기만큼 수신을 시도해 범위를 벗어날 가능성이 있었다.
- 원인: Send 완료 후 `WSABUF.len`만 복구하고 `buf`를 원래 배열 시작점으로 되돌리지 않았다.
- 해결: 세션별 송신 큐의 offset으로 남은 범위를 관리하고, 각 Send에 별도 context와
  `OVERLAPPED`를 사용하도록 변경했다.
- 확인: `V6_FORCE_PARTIAL_SEND_TEST` 빌드에서 한 번의 `WSASend()` 요청을 1 KiB로 제한했다.
  느린 수신 client의 4 MiB echo가 완전히 일치했고 send continuation 4,032회를 확인했다.

#### pending I/O보다 worker와 context를 먼저 정리함

- 증상: 서버 종료 시 worker와 IOCP를 먼저 종료한 뒤 pending I/O가 참조하는 context를 직접
  삭제해 use-after-free가 발생할 수 있었다.
- 원인: socket 취소, completion 회수, context 삭제의 순서가 보장되지 않았다.
- 해결: 종료 상태에서 후속 I/O 게시를 막고, 모든 context socket을 닫아 pending 작업을
  취소한 뒤 condition variable로 context 목록이 빌 때까지 기다린다. 이후 null completion
  패킷으로 worker를 종료하고 IOCP handle을 닫는다.
- 확인: pending `WSARecv()`가 있는 상태에서 일반 빌드와 AddressSanitizer 빌드를 종료해
  completion drain, 모든 worker join, 종료 코드 0과 sanitizer 오류 없음을 확인했다.

#### pending `WSASend()`가 세션 송신 큐의 해제된 메모리를 참조할 수 있었음

- 증상: 정상 echo에서는 드러나지 않았지만, 송신이 pending인 상태에서 `Close_Session()`이
  `send_queue`를 비우면 Windows가 아직 참조할 수 있는 `WSABUF.buf`가 무효화될 수 있었다.
- 원인: Send context는 completion까지 유지됐지만 실제 전송 버퍼는 context가 아니라 세션의
  송신 큐가 소유했다. context 수명만 보장하고 I/O buffer 수명은 별도로 보장하지 못했다.
- 해결: 아직 보내지 않은 데이터를 Send context의 자체 버퍼로 복사하고 `WSABUF.buf`가 해당
  버퍼를 가리키도록 변경했다. completion마다 세션 offset을 갱신하고 다음 context가 남은 범위를
  다시 복사하는 기존 partial-send 흐름은 유지했다.
- 확인: GitHub Actions Windows runner에서 일반 echo, 강제 partial-send, MSVC AddressSanitizer
  작업이 모두 성공했다. 큰 데이터를 보낸 뒤 응답을 읽지 않은 상태의 서버 종료에서도 sanitizer
  오류 없이 context drain과 정상 종료를 확인했다.

#### `Stop()`과 `Post_Accept()`가 `listen_socket`을 서로 다른 규칙으로 접근함

- 증상: 종료 요청과 Accept 게시가 겹치면 `Post_Accept()`가 `listen_socket`을 읽는 동안
  `Stop()`이 같은 멤버를 `INVALID_SOCKET`으로 바꿀 수 있었다.
- 원인: `Post_Accept()`는 context mutex를 사용하고 `Stop()`은 server mutex를 사용해 일반
  변수인 `listen_socket`의 동시 read/write를 같은 동기화 규칙으로 보호하지 않았다.
- 해결: `Stop()`은 `Stopping` 상태 전환과 condition variable 알림만 담당하고, 실제 socket
  정리는 `Clean_Up()`에서만 수행하도록 소유권을 한 곳으로 모았다.
- 확인: GitHub Actions의 세 Windows 작업에서 pending-send 종료가 30초 안에 완료되고 프로세스
  종료 코드 0을 확인했다.

#### 실행 중 실패한 `AcceptEx`를 보충하지 않아 accept depth가 감소함

- 증상: 실행 중 `AcceptEx` completion이 실패할 때마다 선게시된 Accept 작업 수가 16개에서
  하나씩 감소할 수 있었다.
- 원인: 종료 중 취소 completion과 실행 중 실패 completion을 같은 분기에서 정리하고 반환했다.
- 해결: 서버가 `Running`이 아닌 경우에는 정리만 하고, `Running` 상태의 실패라면 accept socket과
  context를 정리한 뒤 `Post_Accept()`를 호출해 대체 작업을 게시하도록 분리했다.
- 확인: Windows CI의 일반/강제 partial-send/ASan 작업에서 빌드, echo, 종료 회귀 테스트를 통과했다.
