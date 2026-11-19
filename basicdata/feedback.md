# 버전별 피드백

각 버전을 구현한 뒤, 현재 구조에서 아쉬웠던 점과 다음 버전 또는 추후 리팩터링에서
보완하면 좋을 점을 기록한다.

이 문서는 버그 해결 과정이나 개념 설명보다 **구현을 끝낸 뒤의 회고**에 초점을 둔다.

- 버그의 증상·재현·해결 과정: [troubleshooting.md](troubleshooting.md)
- 구현하면서 이해한 개념: [learnings.md](learnings.md)
- 버전별 성능 측정: [benchmark.md](benchmark.md)

각 피드백은 다음 상태 중 하나로 관리한다.

- `미적용`: 아직 반영하지 않음
- `다음 버전`: 다음 버전의 구조에서 해결할 예정
- `의도적 유지`: 해당 버전의 학습 목적상 단순한 구조를 유지
- `완료`: 현재 또는 이후 버전에서 반영함

## 반복 피드백 기록 규칙

- 구현 중 리뷰는 이 문서에 기록하지 않는다.
- 해당 버전의 구현과 검증이 완료된 뒤에만 최종 피드백을 기록한다.
- 완료된 버전을 사용자가 수정한 뒤 다시 피드백을 요청한 경우:
  - 기존의 `보완하면 좋았을 점` 설명은 삭제하지 않는다.
  - 변경 내용이 기존 피드백 항목과 관련 있다면 해당 항목 아래에 `보완 내용`을 추가한다.
  - `보완 내용`에는 실제로 변경된 부분과 빌드·테스트 등으로 확인한 결과를 기록한다.
  - 구현과 검증이 끝난 항목의 상태 표시는 `[미적용]`에서 `[완료]`로 갱신한다.
  - 일부만 개선됐다면 남은 문제도 함께 기록한다.
  - 기존 항목과 관련 없는 새로운 문제는 별도의 `보완하면 좋았을 점`으로 추가한다.

기록 예시:

```md
- `[미적용]` `send()` 반환값을 확인하고 partial send를 처리해야 한다.
  - **보완 내용 (YYYY-MM-DD):** 반복 전송 로직을 추가해 받은 길이만큼 모두 전송하도록 변경했다.
    빌드 성공을 확인했으며, partial send를 강제로 재현하는 테스트는 아직 진행하지 않았다.
```

---

## v1 — 싱글 스레드 blocking TCP 서버

### 보완하면 좋았을 점

- `[의도적 유지]` 한 클라이언트의 `recv()`가 끝날 때까지 다음 클라이언트를 처리할 수 없다.
  - v1은 blocking 동작의 한계를 확인하는 버전이므로 그대로 유지한다.
  - v2의 thread-per-connection 구조에서 보완한다.

- `[완료]` `send()`의 반환값을 확인하고 partial send를 처리하면 더 안전하다.
  - 현재는 작은 echo 메시지를 전제로 `send()`를 한 번만 호출한다.
  - 전송한 크기가 받은 크기보다 작으면 남은 범위를 다시 전송하는 `Send_All()` 형태로 보완할 수 있다.
  - **보완 내용 (2026-07-25):** `Send_All()`을 추가해 실제 전송된 바이트 수를 누적하고,
    남은 시작 주소와 길이로 `send()`를 반복하도록 변경했다. `EINTR` 발생 시 같은 범위를
    재시도하고, 그 밖의 실패는 호출부에 전달해 클라이언트 fd를 닫도록 처리했다.
    `v1_server` 빌드와 `nc`를 이용한 기본 echo 회귀 테스트는 성공했다. 로컬 환경에서
    partial send 자체를 강제로 발생시키는 테스트는 진행하지 않았다.

- `[완료]` `recv()` 실패와 정상 종료를 분리하면 오류 원인을 더 정확히 기록할 수 있다.
  - `recv_result == 0`: 상대방이 정상적으로 연결을 종료함.
  - `recv_result < 0`: 실제 오류이며 `errno` 확인이 필요함.
  - **보완 내용 (2026-07-25):** `recv_result == 0`의 정상 FIN 종료와 `< 0`의 오류를
    별도 분기로 나눴다. 오류 중 `EINTR`는 연결을 닫지 않고 `recv()`를 재시도하며,
    그 밖의 오류만 `perror()`로 기록한 뒤 client fd를 닫도록 변경했다.
    `v1_server` 빌드와 `nc` echo 회귀 테스트를 통과했다.

- `[완료]` 수신 버퍼와 partial send의 남은 범위를 C 문자열처럼 출력하면 널 종료가
  보장되지 않아 유효 범위 밖의 데이터가 로그에 포함될 수 있었다.
  - **보완 내용 (2026-07-25):** `operator<<` 대신 `cout.write()`를 사용해 각각
    `recv_result`와 `send_len` 범위만 출력하도록 변경했다. 실행 로그에서 기존에 나타났던
    쓰레기 값이 더 이상 출력되지 않는 것을 확인했다.

- `[완료]` `socket()`과 `bind()` 사이에 `SO_REUSEADDR`를 설정하면 서버를 종료한 직후
  같은 포트로 재실행할 때 발생할 수 있는 바인딩 실패를 줄일 수 있다.
  - **보완 내용 (2026-07-25):** `socket()` 성공 후 `bind()` 전에 `setsockopt()`로
    `SO_REUSEADDR`를 활성화하고 반환값을 검사하도록 변경했다. `v1_server` 빌드,
    echo 연결 생성, 서버 종료 직후 같은 9000번 포트 재실행까지 성공했다.

---

## v2 — thread-per-connection

### 잘된 점

- `accept()`로 얻은 클라이언트 fd마다 별도 `std::thread`를 생성해 여러 연결을 동시에 처리한다.
- 비정적 멤버 함수에 함수 포인터, `this`, 클라이언트 fd를 전달하는 방식을 적용했다.
- 리스닝 소켓과 클라이언트 소켓의 역할 및 정리 위치를 구분했다.
- 로그 출력 시 수신 길이만큼 `std::string`을 생성해 버퍼의 널 종료 여부에 의존하지 않는다.

### 보완하면 좋았을 점

- `[완료]` `send()` 반환값을 확인하고 partial send를 처리하는 반복 전송 로직이 필요하다.
  - **보완 내용 (2026-07-25):** v1의 `Send_All()` 패턴을 재사용해 실제 전송량을 누적하고
    남은 주소와 길이를 반복 전송하도록 변경했다. `EINTR`는 재시도하고, 그 밖의 실패는
    호출부에 전달해 해당 client fd를 닫도록 처리했다. 이전의 단일 `send()` 코드는 구현
    차이를 비교할 수 있도록 주석으로 유지했다. `v2_server` 빌드와 두 클라이언트의 동시
    echo 회귀 테스트를 통과했다. partial send 자체를 강제로 발생시키는 테스트는 진행하지 않았다.

- `[완료]` `recv_result == 0`과 `recv_result < 0`을 분리하고, `EINTR`처럼 재시도할 수 있는
  오류를 구분하면 좋다.
  - **보완 내용 (2026-07-25):** 정상 FIN 종료와 실제 수신 오류를 별도 분기로 나누고,
    `EINTR`는 연결을 닫지 않고 `recv()`를 재시도하도록 변경했다. 그 밖의 오류만
    `perror()`로 기록하고 해당 client fd를 닫는다. `v2_server` 빌드와 두 클라이언트의
    echo 회귀 테스트를 통과했다.

- `[완료]` `SO_REUSEADDR`를 `bind()` 전에 설정하면 개발 중 서버 재실행이 편해진다.
  - **보완 내용 (2026-07-25):** `socket()` 성공 후 유효한 listen fd를 저장하고,
    `bind()` 전에 `setsockopt()`로 `SO_REUSEADDR`를 활성화하도록 변경했다.
    `v2_server` 빌드, echo 연결 생성, 서버 종료 직후 같은 9000번 포트 재실행까지 성공했다.

- `[의도적 유지]` 접속마다 스레드를 새로 만들기 때문에 접속 수가 많아질수록 스레드 생성 비용과
  스택 메모리 사용량이 증가한다.
  - **v3에서 해결할 문제:** 고정 크기 thread pool과 작업 큐를 사용해 접속 수와
    worker 스레드 수를 분리한다.
  - **보완 내용 (2026-07-25):** v3에서 고정 worker와 작업 큐를 구현해 worker 수가
    접속 수에 따라 증가하지 않도록 보완했다. v2에는 구조 비교를 위해 thread-per-connection
    방식을 그대로 유지한다.

- `[의도적 유지]` detach된 스레드는 서버 종료 시 기다리거나 회수할 수 없다.
  - 서버 객체가 파괴되는데 워커가 계속 `this`를 사용하면 수명 문제가 발생할 수 있다.
  - **v3에서 해결할 문제:** worker 스레드를 컨테이너가 소유하고, 종료 상태와
    `notify_all()` 및 `join()` 순서를 적용해 스레드를 회수한다.
  - **보완 내용 (2026-07-25):** v3에서는 고정 worker를 `workers` 컨테이너에 보관하고
    종료 시 join하는 기본 구조를 구현했다. v2에서는 접속마다 스레드를 생성하고 detach하는
    단순 구조와 그 한계를 관찰하기 위해 현재 방식을 유지한다.

- `[의도적 유지]` 여러 스레드가 동시에 `std::cout`에 출력하면 로그가 섞일 수 있다.
  - 공유 로그를 두고 mutex 적용 전후를 비교하면 race condition과 동기화 비용을 학습하기 좋다.
  - **보완 내용 (2026-07-25):** 검토 후 지금은 고치지 않기로 함. 테스트용으로 자주 쓰는 서버가 아니고, 오히려 로그에 client fd 번호가 함께 찍혀서 어떤 클라이언트가 보낸 메시지인지 구분하는 데 도움이 된다고 판단함. mutex 적용 실습은 이후 실제 공유 자원(접속자 목록 등)을 다룰 때 진행하기로 함.

- `[완료]` 실제 공유 자원과 mutex 사용 예제가 아직 없다.
  - 전체 접속자 목록 또는 처리한 메시지 수 같은 공유 상태를 추가하고
    ThreadSanitizer로 동기화 전후를 비교할 수 있다.
  - **보완 내용 (2026-07-25):** `std::unordered_set<int>`로 활성 client fd 목록을 만들고
    accept 스레드의 등록과 worker 스레드의 제거를 같은 mutex로 보호했다. `Close_Client()`가
    목록 제거와 `close()`를 한 임계 영역에서 처리해 fd 번호 재사용으로 새 연결 기록이
    지워지는 race를 방지했다. 연결 수 로그로 `0 → 1 → 2 → 1 → 0` 변화를 확인했다.

- `[완료]` 다중 클라이언트 동시 접속, 급격한 접속·종료, 스레드 누수 여부에 대한 QA 기록이 필요하다.
  - **보완 내용 (2026-07-25):** 두 클라이언트를 동시에 연결한 상태에서 각각 echo를
    확인하고 순서대로 종료해 활성 목록이 0으로 돌아오는 것을 검증했다. 이어서 50개 연결을
    최대 10개씩 병렬로 생성·echo·종료했으며 모두 성공했다. 테스트 종료 후 활성 count는 0,
    `lsof`에는 listen fd만 남았고 완료된 worker 스레드도 남아 있지 않았다.

---

## v3 — 고정 크기 thread pool + 작업 큐

### 완료 피드백 (2026-07-27, 보완 완료 2026-07-28)

### 잘된 점

- accept 스레드는 클라이언트 fd를 큐에 넣는 생산자 역할만 하고, 미리 생성한 고정 개수
  worker가 소비자 역할을 수행하도록 v2와 책임을 분리했다.
- `std::queue`, `std::mutex`, `std::condition_variable`을 조합해 공유 작업 큐를 구성했다.
- 큐가 비었을 때 worker가 busy-wait하지 않고 `condition_variable::wait()`에서 잠들도록 했다.
- `wait()`의 predicate를 사용해 spurious wakeup과 종료 요청을 함께 처리했다.
- 큐에서 fd를 꺼내는 동안에만 mutex를 보유하고, 오래 걸리는 `Process_Client()` 실행 전에
  잠금을 해제해 다른 worker가 큐에 접근할 수 있도록 했다.
- worker 스레드를 접속마다 생성하지 않고 재사용하며, `workers` 컨테이너에서 소유하도록 했다.
- 종료 상태, `notify_all()`, `join()`의 순서를 적용해 worker 정리의 기본 구조를 구현했다.
- atomic 카운터와 별도 monitor 스레드를 추가해 30초마다 sleeping, awake, queued 상태를
  관찰할 수 있게 했다.
- worker 수를 초과한 장기 연결이 큐에서 대기한다는 blocking thread pool의 한계를
  실제 상태 출력으로 확인할 수 있게 했다.
- v3 실행 파일을 최상위 CMake 빌드에 연결했으며 `v3_server` 타깃 빌드 성공을 확인했다.

### 보완하면 좋았을 점

- `[의도적 유지]` worker가 클라이언트 연결 전체를 담당하므로 blocking `recv()` 중에는
  해당 worker가 계속 점유된다.
  - worker 4개가 장기 연결에 점유되면 이후 연결은 큐에서 기다린다.
  - 이는 v3에서 확인하려는 핵심 한계이며 v4의 epoll 기반 이벤트 루프에서 보완한다.

- `[의도적 유지]` 동작 관찰을 위해 worker의 lock, wait, pop, recv, send 과정과 서버의
  시작·종료 로그를 상세하게 출력한다.

- `[완료]` `Server_Run()`이 `while(true)`로 accept를 반복하므로 정상적인 종료 요청 경로가 없다.
  - 기본 `Ctrl+C` 종료에서는 C++ 객체 소멸과 `Stopping` 출력이 보장되지 않는다.
  - 별도 `Stop()` 또는 시그널 처리까지 추가해야 완전한 graceful shutdown이 된다.
  - **보완 내용 (2026-07-28):** listen fd를 non-blocking으로 설정하고 `poll()`을 100ms
    timeout으로 사용해 macOS의 listen `shutdown()` 동작에 의존하지 않고 종료 상태를
    확인하도록 변경했다. accept된 client fd는 v3의 blocking worker 구조를 유지하도록
    다시 blocking 모드로 설정한다. `main()`은 SIGINT와 SIGTERM을 `sigwait()`로 동기
    처리하고 `Stop()` 호출 후 server thread를 join하며, 소멸자는 `Stop()`을 재사용한 뒤
    worker와 monitor를 join하고 listen fd를 한 번만 닫는다. worker 4개보다 많은 6개의
    장기 연결을 유지한 상태에서 `Ctrl+C`를 보냈을 때 처리 중인 fd와 큐 대기 fd가 모두
    정리되어 활성 count가 0으로 돌아왔고, `Stopping` 출력과 프로세스 종료가 3초 이내에
    완료됐다.

- `[의도적 유지]` 활성 클라이언트를 처리하는 worker가 blocking `recv()`에 있으면 소멸자의
  `join()`이 클라이언트 연결 종료까지 기다릴 수 있다.
  - **v4에서 해결할 문제:** epoll 이벤트 루프가 준비된 소켓만 처리하도록 바꿔 worker가
    연결 수명 전체를 blocking `recv()`에서 기다리지 않게 한다.
  - **보완 내용 (2026-07-28):** worker가 연결 수명 동안 blocking `recv()`에 점유되는
    구조적 한계는 v4와 비교하기 위해 유지한다. 다만 서버 종료 시에는 `Stop()`이 활성
    client fd를 `shutdown()`해 `recv()`를 깨우므로 client가 먼저 연결을 닫을 때까지
    worker join이 기다리는 문제는 해결했다. 활성 연결 6개 상태의 종료 테스트에서 모든
    client와 worker가 정리되는 것을 확인했다.

- `[완료]` monitor가 `sleep_for(30초)`로 대기하므로 정상 종료 시 monitor `join()`이
  최대 약 30초 지연될 수 있다.
  - **보완 내용 (2026-07-28):** 고정 `sleep_for(30초)`를 `queue_condition.wait_for()`로
    변경했다. 평상시에는 30초 주기로 상태를 출력하지만 `Stop()`의 `notify_all()`과
    `stopping` predicate에는 즉시 반응한다. 6개의 장기 연결을 유지한 종료 테스트에서
    monitor의 30초 대기 없이 전체 프로세스가 3초 이내에 종료되는 것을 확인했다.

- `[완료]` `Enqueue_Client()`와 `Monitor_Workers()`에서 mutex를 직접 `lock()`/`unlock()`한다.
  - 짧은 임계 영역은 `std::lock_guard`로 관리하면 중간 예외나 조기 반환에도 안전하다.
  - **보완 내용 (2026-07-28):** 두 함수의 수동 잠금을 `std::lock_guard` scope로 변경했다.
    `Enqueue_Client()`는 잠금 해제 후 `notify_one()`을 호출하고, `Monitor_Workers()`는
    잠금 중 공유 상태를 지역 변수로 복사하도록 구성했다. `v3_server` 빌드와
    `git diff --check`를 통과했다.

- `[완료]` `send()` 반환값을 확인하지 않아 partial send와 전송 오류를 처리하지 않는다.
  - 작은 echo 메시지를 이용한 구조 학습 단계에서는 유지하되, 실제 프로토콜 구현 전에는
    반복 전송 또는 송신 버퍼 구조가 필요하다.
  - **보완 내용 (2026-07-28):** v2의 `Send_All()` 선언과 구현을 v3 클래스에 맞춰
    그대로 재사용했다. 실제 전송량을 누적해 남은 주소와 길이를 반복 전송하고, `EINTR`는
    재시도하며 그 밖의 실패는 호출부에 전달해 client fd를 닫는다. `v3_server` 빌드와
    두 클라이언트의 echo 회귀 테스트를 통과했다. partial send 강제 재현 테스트는 진행하지 않았다.

- `[완료]` `recv()`의 `EINTR`처럼 재시도 가능한 오류와 연결을 종료해야 하는 오류를
  구분하지 않는다.
  - **보완 내용 (2026-07-28):** `recv_result < 0`의 실제 오류와 `== 0`의 정상 FIN 종료를
    별도 분기로 나눴다. 오류 중 `EINTR`는 재시도하고, 그 밖의 오류는 `perror()` 기록 후
    client fd를 닫는다. `v3_server` 빌드와 두 클라이언트의 echo 및 정상 종료를 확인했다.

- `[완료]` 작업 큐 크기에 제한이 없어 worker보다 빠르게 연결이 유입되면 대기 fd가
  계속 누적될 수 있다.
  - 최대 큐 크기와 초과 연결 처리 정책을 두는 bounded queue를 이후 실험할 수 있다.
  - **보완 내용 (2026-07-28):** 최대 큐 크기를 128로 제한하고 `Enqueue_Client()`가
    종료 중이거나 큐가 가득 찬 경우 `false`를 반환하도록 변경했다. accept 스레드는
    초과 client를 활성 목록에서 제거하고 즉시 닫는다. 장기 연결 140개를 생성한 포화
    테스트에서 worker 처리 4개와 큐 대기 128개, 총 132개가 유지됐고 초과 8개는
    서버에서 닫힌 것을 확인했다.

- `[완료]` `SO_REUSEADDR`가 없어 개발 중 서버를 종료한 직후 같은 포트로 재실행할 때
  `bind()`가 실패할 수 있다.
  - **보완 내용 (2026-07-28):** v2의 `SO_REUSEADDR` 설정 블록을 v3의 `socket()` 성공 후,
    `bind()` 전 위치에 그대로 재사용했다. `setsockopt()` 반환값을 검사하며, `v3_server`
    빌드, echo 연결 생성, 종료 직후 같은 9000번 포트 재실행까지 성공했다.

- `[완료]` 로드맵에 적힌 ThreadSanitizer와 다중 접속 부하 수치가 별도 결과로 기록되지 않았다.
  - 이후 공통 부하 클라이언트를 만든 뒤 v2와 worker 수, 메모리, 처리량을 같은 조건에서
    비교하는 것이 좋다.
  - **보완 내용 (2026-07-28):** AppleClang의 `-fsanitize=thread` 옵션으로 v3 전용 빌드에
    성공했고, 동시 50개 echo 요청은 50/50 성공했다. TSan은 여러 스레드가 공유
    `std::cout`에 동시에 쓰는 지점에서 data race를 보고했으며, 이는 위에서
    `[의도적 유지]`로 분류한 혼합 로그 문제와 일치한다. 같은 로컬 환경의 비-TSan 단일
    표본에서 동시 50개 요청은 v2가 50/50 성공, 총 15.1ms, 평균 5.1ms, p95 9.9ms였고,
    v3가 50/50 성공, 총 17.8ms, 평균 3.6ms, p95 5.6ms였다. 상세 로그가 활성화된 단일
    실행 결과이므로 일반적인 성능 결론이 아니라 회귀 확인용 수치로 기록한다.

## v4 — epoll 기반 이벤트 루프

### 완료 피드백 (2026-07-30)

### 잘된 점

- v3의 blocking worker 구조에서 벗어나, 하나의 Linux epoll 이벤트 루프가 listen fd,
  client fd, stop event fd를 함께 감시하도록 구조를 바꿨다.
- `event_fd == listen_fd`, `event_fd == stop_event_fd`, 그 외 client fd 분기로 fd 역할을
  명확히 나누었다.
- client fd를 `std::unordered_set<int>`로 추적하고, disconnect/error/send fail 시
  epoll 감시 목록에서 제거한 뒤 close하도록 정리했다.
- SIGINT/SIGTERM을 main 스레드의 `sigwait()`에서 받고, `eventfd` write로 `epoll_wait()`를
  깨우는 graceful shutdown을 구현했다.
- `Stop()`과 `CleanUp()`의 책임을 분리하고, cleanup 중복 호출을 막는 플래그를 추가했다.
- Docker Ubuntu 24.04 환경에서 빌드와 host 포트 매핑 테스트를 수행해 Linux epoll 버전의
  실제 실행 환경을 확인했다.

### 보완하면 좋았을 점

- `[의도적 유지]` 현재 client fd와 `SendAll()`은 blocking 동작을 전제로 한다.
  - v4는 epoll의 readiness 이벤트 흐름을 익히는 버전이라 blocking send를 유지했다.
  - 느린 client에 대한 `send()`가 오래 걸리면 단일 event loop 전체가 지연될 수 있다.
  - **v5 또는 v4 확장 실험에서 해결할 문제:** client fd를 non-blocking으로 설정하고,
    `EPOLLOUT`과 session별 send buffer를 도입한다.

- `[의도적 유지]` level-triggered epoll만 사용했다.
  - level-triggered는 학습 초기에 이벤트 반복 원인을 관찰하기 쉽고, accept/recv로 준비 상태를
    소비해야 한다는 점을 확인하기 좋다.
  - **추후 실험:** edge-triggered(`EPOLLET`)를 적용할 경우 non-blocking fd와 `EAGAIN`까지
    반복해서 읽는 drain loop가 필요하다.

- `[완료]` 지속 부하 기준의 성능 측정이 아직 없다.
  - 5개 동시 `nc` echo는 기능 검증에는 충분하지만, v2/v3와 성능을 비교하기에는 표본이 작다.
  - 공통 부하 클라이언트를 만들어 동시 접속 수, 메시지 처리량, 평균/p95/p99 지연시간,
    메모리 사용량을 같은 조건에서 측정하면 좋다.
  - **보완 내용 (2026-07-31):** Docker Ubuntu 24.04 환경에서 `v4_server` 빌드 후,
    50개 client가 각각 20개 메시지를 보내는 방식으로 총 1000개 echo 요청을 실행했다.
    1000/1000 응답 성공, client accepted 50개, closed 50개, stop event 1개를 확인했다.
    총 소요 시간은 3063ms였고 단순 계산 TPS는 약 326 echo/sec였다. 상세 디버그 로그가
    활성화된 단일 실행 표본이므로 정밀 성능 결론이 아니라 v4 부하 회귀 확인용 수치로 기록한다.
    평균/p95/p99 지연시간과 메모리 사용량은 아직 별도 측정하지 않았다.

- `[완료]` `Stop()`과 `CleanUp()`의 동시 호출 경계는 학습 단계 수준으로만 정리되어 있다.
  - 현재 테스트에서는 정상 종료됐지만, 더 엄밀하게는 상태 전이를 `running/stopping/stopped`처럼
    명확히 나누거나 fd 소유권을 RAII 객체로 감싸면 좋다.
  - 기존에는 코드 복잡도를 낮추기 위해 bool 플래그와 mutex로 관리했다.
  - **보완 내용 (2026-08-01):** `Created -> Running -> Stopping -> Cleaning -> Stopped`
    흐름의 `ServerState` enum을 추가하고 `std::atomic<ServerState>`로 서버 생명주기를
    관리하도록 변경했다. `Server_Run()`은 `Created -> Running`, `Stop()`은
    `Running -> Stopping`, `CleanUp()`은 `Stopping -> Cleaning -> Stopped` 전이를
    `compare_exchange_strong()`과 `store()`로 처리해 중복 종료 요청과 중복 fd 정리를
    더 명확히 구분했다. Docker Ubuntu 환경에서 `v4_server` 빌드와 기본 `nc` echo 테스트,
    SIGINT 종료 흐름을 확인했다.

## v5 — 이벤트 루프 + 워커 스레드 풀

### 완료 피드백 (2026-08-07)

### 잘된 점

- v3의 고정 워커 풀과 v4의 epoll reactor를 결합해서, reactor는 accept/recv와
  `EPOLLOUT` 기반 송신만 담당하고 워커는 job queue에서 받은 데이터를 세션별
  송신 버퍼에 적재하는 역할로 분리했다.
- client fd를 non-blocking으로 설정하고 `EPOLLOUT` + 세션별 송신 버퍼(`send_buffers`)를
  도입해, v4 피드백에서 v5 과제로 남겨뒀던 "client fd non-blocking + EPOLLOUT + 세션별
  송신 버퍼"를 그대로 구현했다.
- `Send_All()`을 `bool` 대신 `SendState`(Completed/Partial/Failed) enum으로 재설계해서
  non-blocking 소켓의 `EAGAIN`(아직 다 못 보냄)과 진짜 에러를 구분하고, 못다 보낸 만큼만
  세션 버퍼에 남기도록 만들었다.
- 워커(`Process_Job`)는 세션 버퍼에 append하고 `epoll_ctl(EPOLL_CTL_MOD)`로 `EPOLLOUT`
  관심만 등록하며, 실제 `send()`는 reactor의 `EPOLLOUT` 핸들러가 전담하도록 분리해
  "reactor는 I/O만 담당" 설계를 지켰다.
- 코드 리뷰 과정에서 실제 버그를 여러 개 찾아 수정했다:
  - `Delete_Client_Fd()`에서 `epoll_ctl(EPOLL_CTL_DEL)`과 `close()`가 client_fds 정리,
    send_buffers 정리 각각에서 중복 호출되던 이중 close 버그. fd가 재사용된 경우 엉뚱한
    연결을 끊을 수 있었는데, "실제로 찾아서 지웠는지" 상태를 하나로 모아 정리는 한 번만
    실행하도록 수정했다.
  - `Process_Job()`과 reactor의 `EPOLLOUT` `Failed` 분기 두 곳에서 `send_mutex`를 쥔 채로
    `Delete_Client_Fd()`를 호출해 같은 뮤텍스를 다시 잠그려던 데드락을 발견해, 플래그로
    락 스코프 밖으로 빼서 해결했다.
  - `Send_All()`의 `EAGAIN` 처리에서 `data.erase(total_sent)`가 보낸 부분을 남기고 못
    보낸 부분을 지우는 반대 동작 버그, `SendState::Partial`이 선언만 되고 실제로는
    리턴되지 않던 버그를 발견해 수정했다.
- worker/monitor 스레드를 실제로 생성하고 `Clean_Up()`에서 join하도록 완성했다.
  `Monitor_Workers()`는 v3와 동일하게 `wait_for()` + 종료 predicate로 구현해, 정상
  종료 시 30초 대기 없이 즉시 반응하도록 만들었다.
- 빌드(`-Wall -Wextra` 경고 없음), 단일/동시 접속 echo, `SIGTERM` graceful shutdown을
  실제로 실행해 확인했다.
- 부하 테스트: v4와 동일 시나리오(50 clients × 20 msg)에서 1000/1000 성공, 2000 clients
  × 20 msg(총 40,000 echo) 순간 부하에서도 40000/40000 성공, 300 clients를 30초 이상에
  걸쳐 천천히 늘리는 지속 부하에서도 3000/3000 성공했다.
- `top -H`로 스레드별 CPU를 측정해 job queue 자체는 병목이 아니며(대부분 `Queued: 0`),
  reactor 스레드 하나가 모든 I/O를 처리하는 구조가 실제 병목이라는 걸 확인했다 — v5가
  의도한 설계("reactor는 I/O만 담당")의 당연한 한계다.
- AddressSanitizer + LeakSanitizer로 120개 연결, 1800개 echo 규모의 반복 연결/해제
  부하 후 정상 종료까지 확인했고, 메모리 누수나 메모리 안전성 에러는 발견되지 않았다.

### 보완하면 좋았을 점

- `[완료]` 여러 스레드(reactor, 워커, monitor)가 동시에 `std::cout`에 출력하면 로그가
  섞일 수 있다는 문제는 v2에서 이미 `[의도적 유지]`로 남겨뒀었다. v5에서는 동시성 규모가
  커지면서(300개 동시 접속 부하 테스트) 단순히 줄이 섞이는 수준을 넘어 `Sending to client`
  로그의 fd 값 자체가 `6281472909701472`처럼 깨진 숫자로 출력되는 것까지 실제로 확인됐다.
  - **보완 내용 (2026-08-07):** 로그 전용 `log_mutex`와 `Logging()` 함수를 추가해, 모든
    출력이 이 함수 하나만 거치도록 통일했다. 호출부는 `std::ostringstream`으로 한 줄
    전체를 완성한 뒤 `Logging()`을 한 번만 호출해서, 한 줄 안에서 출력이 여러 스레드로
    쪼개지는 일이 없게 했다(`Send_All()`처럼 원래 `cout` 호출이 여러 번 나뉘어 있던 로그도
    포함). 내부 구현은 `printf("%s\n", msg.c_str())`를 쓰는데, 처음엔 `printf(msg.c_str())`
    형태로 하려다가 클라이언트가 보낸 데이터가 그대로 로그 메시지에 섞여 들어가는 경우
    (`Sending to client`) 그 데이터가 포맷 문자열로 해석되는 format string 취약점이 될
    수 있어서, 포맷 문자열은 `"%s\n"`으로 고정하고 실제 메시지는 인자로만 넘기도록
    수정했다. `v5_server` 빌드 성공, 로그가 깨졌던 것과 같은 2000개 동시 접속 부하
    (40000/40000 echo 성공) 재현 테스트에서 로그 손상이 더 이상 발생하지 않는 것을
    확인했다.

- `[완료]` 같은 커넥션에서 온 job이 서로 다른 워커에 분산 처리될 수 있어서, 클라이언트가
  응답을 기다리지 않고 메시지를 연달아 보내는 경우(pipelining) 세션 버퍼에 append되는
  순서가 원래 보낸 순서와 뒤바뀔 수 있다. 지금 echo 테스트 클라이언트는 항상 요청-응답을
  기다리는 패턴이라 3000개 요청 부하 테스트에서도 재현되지 않았지만, 구조적으로는 남아있는
  문제였다.
  - **보완 내용 (2026-08-07):** 전역 job queue 하나를 워커별 큐(`Worker` 구조체 —
    뮤텍스+큐+조건변수 묶음)로 나누고, `Enqueue_Job()`이 `client_fd % 워커 수`로 항상
    같은 워커에 라우팅하도록 sticky routing을 구현했다. `Stop()`은 워커별 조건변수를
    모두 `notify_all()`하도록, `Monitor_Workers()`는 모니터 전용 뮤텍스/조건변수를 새로
    만들어 특정 워커의 job 알림과 얽히지 않게 분리하고 워커별 큐 길이를 각자의 뮤텍스로
    보호하며 합산하도록 바꿨다. `v5_server` 빌드 성공, 50 clients×20 msg(1000/1000)와
    300 clients 지속 부하(1500/1500) 회귀 테스트, `Worker Status` 로그 정상 출력,
    `SIGTERM` graceful shutdown까지 확인했다.

- `[완료]` `Clean_Up()`은 종료 시 세션 버퍼에 아직 다 못 보낸 데이터가 남아있어도
  그냥 `close()`로 끊어버린다. 부하 테스트 중 서버 종료 타이밍이 마지막 메시지 처리와
  겹치면 클라이언트가 응답을 못 받고 연결이 끊기는 걸 실제로 재현했다(테스트 하네스의
  `timeout` 값이 실제 소요 시간과 너무 가까워서 우연히 드러났다).
  - **보완 내용 (2026-08-07):** `client_fds`를 닫기 전에 그 fd의 `send_buffers`에 남은
    데이터가 있으면 `Send_All()`을 한 번 더 시도해 최선 노력(best-effort)으로 flush하도록
    바꿨다. non-blocking 소켓이라 커널 송신 버퍼가 꽉 차 있으면 그래도 그냥 닫지만,
    대부분의 경우(작은 echo 메시지 수준) 이 한 번의 시도로 충분히 다 보내진다. 버그를
    처음 재현했던 것과 같은 조건(서버 `timeout` 45초, 300 clients를 100ms 간격으로 30초
    이상 늘리는 지속 부하)을 그대로 다시 돌려서 이번엔 3000/3000 전부 성공하는 것을
    확인했다. 2000 clients × 20 msg(40,000 echo) 부하로도 회귀 테스트해 40000/40000
    성공과 정상 종료를 확인했다.

- `[완료]` `Clean_Up()`에서 `send_buffers` 정리가 `active_client_mutex` 블록 안에서
  이뤄지고 있다. `send_buffers`는 원래 `send_mutex`로 보호하기로 한 멤버라, 이 시점엔
  워커/모니터가 이미 join된 뒤라 실질적인 race는 없지만 "이 멤버는 이 뮤텍스가 보호한다"는
  원칙과는 어긋난다.
  - **보완 내용 (2026-08-07):** `send_buffers` 정리를 `active_client_mutex` 블록에서
    분리해 `send_mutex`로 보호하는 별도 블록으로 옮겼다. `v5_server` 빌드 성공을 확인했다.

## v6 — Windows IOCP 기반 비동기 I/O 서버

### 잘된 점

- `AcceptEx()`를 listen socket의 IOCP completion으로 처리하고, 완료된 client socket을 같은
  completion port에 연결한 뒤 다음 accept를 게시하는 흐름을 구현했다.
- 세션과 작업별 context를 분리하고 세션별 송신 큐로 순서를 보장해, 한 연결의 Recv와 Send가
  동시에 진행되면서도 각 pending I/O의 상태와 수명이 독립적으로 유지된다.
- partial send에서는 송신 큐 offset과 남은 길이를 갱신해 continuation을 게시한다.
- `AcceptEx`의 정상 0바이트 완료와 Recv의 0바이트 연결 종료를 I/O 타입으로 구분했다.
- 종료 시 pending I/O보다 context가 먼저 파괴되지 않도록 socket 취소 → completion drain →
  context 목록 empty → worker 종료 패킷 → join → IOCP 정리 순서를 적용했다.
- 고정 10개였던 worker 기준을 논리 코어 수로 통일하고, 미사용 `Register_Client()` 경로와
  Winsock 관련 컴파일 경고를 정리했다.
- 일반 빌드 2000/2000 echo와 pending receive 종료, MSVC AddressSanitizer 빌드 1000/1000 echo,
  강제 부분 전송 4 MiB echo와 같은 종료 시나리오를 통과했다.

### 보완하면 좋았을 점

- `[완료]` context 하나에 `OVERLAPPED` 하나를 두고 Recv와 Send를 번갈아 게시하므로
  한 세션에서 수신과 송신을 동시에 진행할 수 없다.
  - v6에서는 completion 모델과 context 수명을 익히기 위해 단순한 직렬 echo 흐름을 유지한다.
  - v7에서 세션 객체와 I/O 작업별 context를 분리할 때 참조 수명과 처리 순서를 다시 설계한다.
  - **보완 내용 (2026-08-14):** socket과 송신 큐를 소유하는 `ClientSession`과 Accept/Recv/Send별
    `IoContext`를 분리했다. Recv 완료 직후 다음 Recv를 게시하고 받은 데이터는 세션별 송신 큐에서
    순서대로 처리해, 한 세션의 수신과 송신이 동시에 진행될 수 있도록 변경했다.

- `[완료]` 동시에 pending인 `AcceptEx()`가 하나뿐이라 짧은 시간에 접속이 몰리면 accept
  처리량이 제한될 수 있다.
  - 여러 accept context를 미리 게시하면 접속 폭주 대응이 좋아지지만 context 수명과 종료 시
    취소해야 할 작업 수가 늘어난다.
  - **보완 내용 (2026-08-14):** 서버 시작 시 Accept context 16개를 미리 게시하고, 각 완료 시
    대체 Accept를 다시 게시하도록 변경했다. 200개 동시 연결에서 2,000/2,000 echo 성공을 확인했다.

- `[완료]` `OVERLAPPED*`를 `IoContext*`로 직접 변환하는 방식은 `overlapped`가 구조체의 첫
  멤버라는 배치에 의존한다.
  - 작업별 context를 도입할 때 `CONTAINING_RECORD`와 명시적인 base/owner 관계를 비교할 수 있다.
  - **보완 내용 (2026-08-17):** worker에서 `CONTAINING_RECORD`로 소유 `IoContext`를 찾도록
    변경했다. `overlapped`를 구조체 마지막 멤버로 옮겨 첫 멤버 배치에 의존하지 않음도 확인했다.

- `[완료]` 종료 시 `context_set.empty()`를 제한 없이 기다리므로, 운영 환경에서는 예상하지
  못한 completion 누락을 진단할 timeout과 남은 context 로그가 필요하다.
  - context를 안전하게 임의 삭제할 수는 없으므로 timeout은 강제 삭제보다 장애 진단과 프로세스
    종료 정책을 결정하는 용도로 사용하는 편이 안전하다.
  - **보완 내용 (2026-08-17):** 안전한 completion drain 자체는 유지하면서 5초마다 남은 pending
    context 수를 출력하는 진단 대기를 추가했다. 부하 및 ASan 테스트에서 모두 drain과 정상 종료를 확인했다.

- `[완료]` 연결 단위 상세 로그는 부하가 커질수록 로그 mutex와 콘솔 출력이 처리량 측정을
  왜곡할 수 있다. 실제 벤치마크에서는 로그 레벨을 낮추고 TPS, 평균/p95/p99 지연시간과 메모리를
  별도로 측정해야 한다.
  - **보완 내용 (2026-08-17):** `Debug`, `Info`, `Error` 로그 레벨을 추가하고 기본값을 `Info`로
    설정했다. 연결 단위 종료 로그는 `Debug`로 낮춰 일반 부하 검증에서 콘솔 출력량을 줄였다.

- `[완료]` 로컬 회귀 테스트에서는 partial send 자체를 강제로 발생시키지 못했다. 작은 socket
  send buffer, 큰 payload, 느린 수신 client를 사용하는 별도 시나리오가 필요하다.
  - **보완 내용 (2026-08-17):** `V6_FORCE_PARTIAL_SEND_TEST` CMake 옵션을 추가해 한 번의 실제
    `WSASend()` 요청 크기를 1 KiB로 제한하고, 논리적 송신 길이가 남으면 continuation 경로를
    반복하도록 했다. 느린 수신 client의 4 MiB payload가 완전히 일치했고 continuation 4,032회를
    확인했다.

- `[완료]` Send context가 completion까지 유지되더라도 `WSABUF`가 세션 송신 큐 내부 메모리를
  직접 가리키면, 종료 중 `send_queue.clear()` 이후 pending `WSASend()`가 해제된 메모리를 참조할
  수 있다.
  - **보완 내용 (2026-08-18):** 아직 보내지 않은 범위를 Send `IoContext`의 자체 버퍼로 복사하고
    `WSABUF.buf`가 context 버퍼를 가리키도록 변경했다. GitHub Actions Windows runner의 일반,
    강제 partial-send, MSVC AddressSanitizer 작업에서 echo와 pending-send 종료를 모두 통과했다.

- `[완료]` `Post_Accept()`가 `listen_socket`을 읽는 동안 `Stop()`이 서로 다른 mutex 아래에서
  같은 멤버를 `INVALID_SOCKET`으로 변경해 data race가 발생할 수 있다.
  - **보완 내용 (2026-08-18):** `Stop()`은 상태 전환과 `run_cv` 알림만 담당하고 socket 정리는
    `Clean_Up()` 한 곳에서 수행하도록 책임을 분리했다. 세 Windows CI 작업에서 context drain과
    30초 이내 정상 종료를 확인했다.

- `[완료]` 실행 중 실패한 `AcceptEx` completion을 정리만 하면 선게시 accept depth가 실패할
  때마다 감소한다.
  - **보완 내용 (2026-08-18):** 종료 중 실패는 정리만 하고, `Running` 상태의 실패는 정리 후
    `Post_Accept()`를 호출해 대체 Accept를 게시하도록 분기했다. Windows CI의 일반/강제
    partial-send/ASan 회귀 테스트가 모두 성공했다.

## v7 — IOCP + 워커/game logic queue 결합

### 완료 피드백 (2026-11-19)

### 잘된 점

- v6의 IOCP 구조(세션/작업별 context, 세션별 순서 보장 송신 큐, pending I/O drain 종료)를 그대로
  재사용하고, 그 위에 패킷 조립 → game job 큐 → game logic 스레드 단계를 추가해 네트워크 완료 처리와
  게임 로직을 서로 다른 스레드로 분리했다.
- `[uint16 size][uint16 id][body]` 길이 헤더 프로토콜과 세션별 `PacketAssembler`로 TCP 스트림을 패킷
  단위로 잘랐다. 조립기는 소켓과 분리된 순수 클래스라 단위 테스트(1바이트 분할, 다중 패킷, 비정상 size)로
  검증했다.
- 세션마다 `WSARecv`를 하나만 pending으로 두는 v6 구조 덕분에 조립기에 락 없이 접근할 수 있다는 점을
  설계에 그대로 활용했다.
- game logic 스레드를 하나로 두어 방 상태(`room_members`)를 mutex 없이 다루고, 세션별 처리 순서를 큐
  순서로 보장했다.
- Enter job을 첫 `Post_Recv()` 전에 넣고, Leave는 `closing`을 세운 뒤 넣으며, `Handle_Enter()`가
  `closing`을 다시 확인해 입장/퇴장/패킷의 순서 경쟁을 막았다.
- completion worker(생산자)가 game 로직 때문에 멈추지 않도록 job 큐 `Push()`를 기다리지 않는 bounded
  큐로 만들었다.
- 브로드캐스트 데이터는 `shared_ptr<const vector<char>>`로 공유해 방 인원수만큼 복사하지 않으면서도,
  실제 `WSASend()`에는 v6처럼 context 자체 버퍼로 복사해 pending I/O 버퍼 수명을 지켰다.
- 응답을 읽지 않는 클라이언트를 세션 송신 큐 상한(4096개)으로 끊어 서버 메모리를 보호했다.
- 종료 순서를 "job 큐 Close → logic 스레드 join → v6의 I/O 취소·drain → IOCP worker join"으로 정해,
  송신을 요청하는 쪽을 먼저 멈췄다. `Stop()`은 v6처럼 상태 전환과 알림만 담당한다.
- `Server_Run` 스레드가 Stop을 기다리는 동안 10초마다 세션 수, pending I/O, job 큐 길이와 최고치,
  처리량을 출력하도록 해 별도 monitor 스레드 없이 병목을 관찰할 수 있게 했다.
- 정상/ASan 빌드에서 50 clients(전달 50,000건)와 200 clients(전달 400,000건), 분할 패킷, 비정상 헤더,
  느린 클라이언트, 종료 시나리오를 통과했다. GUI에도 v7 패킷 모드를 추가했다.

### 보완하면 좋았을 점

- `[완료]` game logic 스레드가 방송할 때 모든 멤버에 대해 `Enqueue_Send()`를 호출하고, 그 안에서
  `WSASend()` 게시(시스템 호출)까지 직접 수행한다.
  - 200 clients × 10 msg 부하에서 `job_queue_peak=1997`로, 채팅 2,000개가 거의 전부 큐에 쌓였다.
    v5에서 reactor 하나가 병목이었던 것처럼 v7에서는 logic 스레드 하나가 병목이고, 그 시간의 상당 부분이
    게임 로직이 아니라 송신 게시 비용이다.
  - logic 스레드는 세션 송신 큐에 넣기만 하고 실제 게시는 IOCP 쪽(예: `PostQueuedCompletionStatus()`로
    송신 신호)으로 넘기거나, 큐에 쌓인 여러 패킷을 `WSABUF` 배열 하나로 묶어 보내는 방식을 비교할 수 있다.
  - **보완 내용 (2026-11-19):** 두 방식을 모두 구현해 같은 부하(50 clients × 20 msg, 2회 이상 교대 측정)로 비교했다.
    - IOCP로 넘기기: logic 스레드가 `PostQueuedCompletionStatus()`로 송신 요청만 넣고 worker가
      `WSASend()`를 게시하게 했더니 오히려 느려졌다. 개선 전 33,331~38,825 전달/초(p50 642~768ms)에서
      22,394~27,446 전달/초(p50 957~1,062ms)가 됐다. 단계가 하나 늘었고, 16개 worker가 전역
      `context_mutex`를 두고 경쟁했기 때문이다. 이 방식은 채택하지 않았다.
    - 묶어 보내기: `Post_Next_Send_Locked()`가 큐 맨 앞 패킷 하나가 아니라 큐에 쌓인 여러 패킷을 64 KiB
      context 버퍼에 이어 붙여 `WSASend()` 한 번으로 보내고, completion 바이트만큼 여러 패킷에 걸쳐 큐를
      전진시키도록 바꿨다. 이미 송신 중인 세션에는 logic 스레드가 큐에 넣기만 하고, 그동안 쌓인 패킷은
      Send completion 때 한 번에 나간다.
    - 확인: 묶어 보내기 적용 후 95,887~120,172 전달/초, p50 209~285ms, p99 409~511ms로 약 3배 빨라졌다(측정 5회).
      묶어 보내기만 끈 비교 빌드는 34,344~40,074 전달/초로 개선 전 수준이라, 개선 효과는 묶어 보내기에서
      나왔다. 200 clients × 10 msg에서는 31,935~32,042 → 50,776~55,319 전달/초, p99 12.3초 → 7.1~7.8초였다.
      다만 부하 클라이언트가 채팅 2,000개를 한꺼번에 보내므로 `job_queue_peak`는 1,990으로 거의 같다.
      바뀐 것은 큐를 비우는 속도다. logic 스레드가 하나라는 구조적 상한은 그대로 남아 있다.

- `[완료]` 세션 송신 큐 상한이 바이트가 아니라 메시지 개수(4096개) 기준이다.
  - 최대 크기(8 KiB) 패킷만 쌓이면 세션 하나가 약 32 MiB까지 잡을 수 있다. 바이트 기준 상한이 메모리
    보호 목적에 더 정확하다.
  - **보완 내용 (2026-11-19):** `ClientSession::send_queued_bytes`로 아직 보내지 않은 바이트를 추적하고,
    넣을 때 더하고 전송 완료·세션 종료 때 빼거나 비우게 했다. 상한은 세션당 4 MiB(`kMaxSendQueueBytes`)다.
    응답을 읽지 않는 클라이언트에 1 KB 채팅 12,000개를 방송하는 시나리오에서 그 세션만 `send queue overflow`로
    끊기고 서버가 종료 코드 0으로 끝나는 것을 일반 빌드와 ASan 빌드에서 확인했다.

- `[완료]` job 큐가 가득 차면 패킷을 보낸 세션을 끊는다.
  - 서버 전체가 과부하일 때 정상 클라이언트도 끊긴다. 해당 세션의 다음 `WSARecv()` 게시를 잠시 미루는
    backpressure를 쓰면 TCP 흐름 제어로 클라이언트 송신 속도를 늦출 수 있다.
  - **보완 내용 (2026-11-19):** Recv completion에서 패킷을 큐에 넣은 뒤 job 큐 길이가 높은 수위(기본 8,192) 이상이면
    다음 `WSARecv()`를 게시하지 않고 세션을 일시정지 목록에 넣는다. logic 스레드는 job을 하나 처리할 때마다
    큐 길이가 낮은 수위(기본 1,024) 이하인지 보고, 그렇다면 멈춘 세션들의 Recv를 다시 게시한다. 세션을
    일시정지 목록에 넣은 쪽도 넣은 직후 재개 조건을 한 번 더 확인한다. logic 스레드가 이미 큐를 비우고 잠든
    뒤라도 세션이 영원히 멈춰 있지 않게 하기 위해서다. 최종 상한(65,536)에 닿을 때만 연결을 끊는다.
    수위는 테스트를 위해 `V7_JOB_HIGH_WATERMARK`/`V7_JOB_LOW_WATERMARK` 환경변수로 덮어쓸 수 있다.
  - 확인: 수위를 64/16으로 낮춘 서버에 4개 클라이언트가 작은 채팅 5,000개씩을 몰아 보내자 `recv_pauses=4`,
    `jobs_rejected=0`이었다. 80,000/80,000건이 발신자별 순서대로 전달됐다(일반·ASan 빌드).

- `[완료]` `Logging()`이 `log_mutex`를 쥔 채 `std::cout`에 출력한다.
  - stdout이 막히면(파이프 버퍼 가득 참) 로그를 찍으려는 모든 스레드가 멈추고 서버 전체가 정지하는 것을
    실제로 재현했다(troubleshooting.md 참고). 로그 문자열을 큐에 넣고 별도 스레드가 출력하는 방식이면
    I/O 스레드와 출력 지연을 분리할 수 있다.
  - **보완 내용 (2026-11-19):** `AsyncLogger`를 추가했다. 서버 스레드는 `Write()`로 문자열을 큐에 넣기만 하고,
    writer 스레드가 큐를 통째로 가져가 잠금 밖에서 출력한다. 큐가 상한(100,000줄)을 넘으면 기다리지 않고
    버린 개수만 세며, 종료 시 남은 로그를 모두 출력한 뒤 버린 개수를 한 줄로 남긴다. `[Status]`에 `log_dropped`를 추가했다.
  - 확인: 단위 테스트로 4개 스레드 1,000줄의 줄 단위 무결성·스레드별 순서와 상한 초과 시 버림을 검증했다.
    stdout을 읽지 않는 같은 조건에서 개선 전 빌드는 58번째 클라이언트에서 멈췄고, 개선 후 빌드는 400명 모두 입장했다.

- `[완료]` Recv와 Send마다 64 KiB 버퍼를 가진 `IoContext`를 새로 할당한다(v6에서 이어진 구조).
  - 세션별 Recv context 재사용이나 송신 버퍼 풀로 할당 비용을 줄이고, 그 전후를 같은 부하로 비교할 수 있다.
  - **보완 내용 (2026-11-19):** 세션당 Recv와 Send는 각각 최대 하나만 pending이라는 점을 이용했다.
    `ClientSession`이 Recv/Send context를 하나씩 소유하고 completion 뒤 `OVERLAPPED`를 초기화해 다시
    게시한다. pending 동안에는 context가 `shared_ptr<ClientSession>`로 세션을 붙잡는다. completion 처리
    시작 시 그 참조를 꺼내 순환 참조를 끊는다. Accept context만 접속마다 만든다.
  - 확인: 일반·ASan 빌드의 전체 시나리오(분할 패킷, 비정상 헤더, 느린 클라이언트 종료 포함)에서 오류와
    pending context 누락 없이 종료했다. 묶어 보내기만 끈 비교 빌드가 개선 전과 비슷한 처리량을 보여,
    이 부하에서는 할당 비용 감소가 처리량 차이로 따로 드러나지 않았다.

- `[의도적 유지]` 종료 시 job 큐에 남은 job은 처리하지 않고 버린다(`jobs discarded`).
  - 응답을 보낼 socket이 곧 모두 닫히므로 처리해도 의미가 적다. 저장이 필요한 게임 상태가 생기면
    종료 전 처리 정책을 다시 정해야 한다.

- `[완료]` 로드맵의 "v5 Linux 구조와 동일 부하에서 처리량, 지연시간, 스레드 수, 메모리 비교"를 하지 못했다.
  - v7은 echo가 아니라 방송이라 처리량 단위가 다르고, 부하 클라이언트가 같은 머신의 Python 단일
    프로세스라 클라이언트 쪽이 상한일 수 있다. 요청별 지연시간(p95/p99)도 측정하지 않았다.
  - **보완 내용 (2026-11-19):** 통합 테스트가 각 클라이언트가 자기 채팅을 방송으로 돌려받을 때까지의
    지연시간을 기록해 p50/p95/p99를 출력하도록 했다. v7은 50 clients × 20 msg에서 p50 209ms, p99 412ms였다
    (채팅을 한꺼번에 보내는 부하라 큐 대기 시간이 대부분이다). v5와의 직접 비교는 아직 남아 있다. 이 PC의
    WSL Ubuntu에는 `g++`와 `cmake`가 없어 v5를 빌드할 수 없었다.
  - **보완 내용 (2026-11-19, v5 비교):** WSL Ubuntu 24.04(g++ 13.3, Release)에 v5를 빌드했다. 공통 스크립트
    [`basicdata/tools/compare_v5_v7.py`](tools/compare_v5_v7.py)로 v5와 v7을 같은 방식으로 쟀다. 기능이 달라서
    클라이언트 1명만 연결했다(v7 방에 1명이면 자기 채팅만 돌아오므로 v5 echo와 같은 요청 1개 → 응답 1개가 된다).
    클라이언트는 서버와 같은 OS에서 실행했다. OS·클라이언트 차이를 가늠하려고 같은 OS의 가장 단순한 echo
    서버(Linux v1, Windows v6)도 기준선으로 함께 쟀다. 각 3회 측정했다.
    - 주고받기(왕복 5,000회): v5 4,098~4,935 왕복/초, p50 198~237µs(기준선 v1: 4,149~5,553, p50 155~213µs).
      v7 6,854~8,692 왕복/초, p50 112~133µs(기준선 v6: 8,041~8,410, p50 103~106µs). 구조 때문에 더해지는
      지연은 v5가 약 20~40µs, v7이 약 10~30µs로 둘 다 작다. v7의 절대 수치가 더 좋은 것은 대부분 OS와
      클라이언트 환경 차이다.
    - 연속 전송(32바이트 50,000개): v5 120,471~125,300 메시지/초(기준선 v1 97,185~99,939),
      v7 90,556~104,514 메시지/초(기준선 v6 577,957~675,841). Windows 클라이언트는 단순 echo에서 6배 가까이
      받을 수 있으므로, v7은 서버의 패킷당 처리 비용이 상한이다(아래 새 항목).
    - 유휴 연결 0/100/500/1,000개의 메모리: v5 3.5/3.5/3.5/3.6 MB(스레드 7개), v7 6.1/18.8/69.6/133.2 MB
      (스레드 23개). v6는 7.0/13.4/38.8/70.6 MB(스레드 21개)였다. v7은 연결당 약 127 KB를 쓴다(아래 새 항목).
    - v5 출력을 `/dev/null`로 버려도 주고받기 결과는 3,604~4,452 왕복/초로 비슷했다. 메시지마다 찍는 v5 로그 출력이
      지연의 주원인은 아니었다.

- `[미적용]` v6와 달리 v7용 GitHub Actions 워크플로가 없어 Windows 회귀 테스트가 로컬 실행에만 의존한다.
  - **보완 내용 (2026-11-19):** `.github/workflows/v7-windows.yml`을 추가했다. Release와 AddressSanitizer
    두 작업이 v7을 빌드하고 `v7_unit_tests`와 `v7_smoke_test.py`(50 clients × 20 msg)를 실행한다. 같은
    명령을 로컬에서 실행해 통과를 확인했다. GitHub runner에서의 실제 실행은 push 후 확인이 남아 있다.

- `[완료]` GUI의 v7 패킷 모드는 빌드만 확인했고, 실제 화면에서 여러 클라이언트로 채팅하는 확인은 하지 않았다.
  - **보완 내용 (2026-11-19):** Windows UI Automation으로 실제 `MMO Server Lab` 창에서 v7을 선택해 방을 시작했다.
    클라이언트 3개를 연결한 뒤 Client 1과 Client 2에서 채팅을 보냈다. 개별 화면에 환영·입장 알림과 다른
    클라이언트의 채팅이 표시되고, 공통 채팅에는 두 메시지가 발신자 기준으로 한 번씩만 표시되는 것을
    화면 텍스트와 캡처로 확인했다.

- `[완료]` `start.ps1`은 `server_lab` 테스트를 Release 구성으로 실행하는데, 카탈로그 테스트가 `assert`만
  사용해 Release에서는 아무것도 검사하지 않는다.
  - 기대값이 틀린 이전 테스트가 Release에서 통과하고 Debug에서만 실패하는 것을 재현했다
    (troubleshooting.md 참고). v7 단위 테스트처럼 실패 횟수를 세어 종료 코드로 돌려주는 방식으로 바꿀 수 있다.
  - **보완 내용 (2026-11-19):** `server_catalog_test.cpp`의 `assert`를 실패 줄을 출력하고 개수를 세는 `CHECK`로
    바꾸고, 실패가 있으면 종료 코드 1을 돌려주게 했다. Release 구성에서 통과를 확인했다. 기대값을 일부러
    틀린 사본은 Release에서도 `FAILED line 28`로 실패했고, 디버그 오류 창도 뜨지 않았다.

- `[완료]` GUI의 `방 종료`가 Windows에서 서버를 정상 종료시키지 못하고 3초 뒤 강제 종료한다.
  - GUI 확인 중 발견했다. `Stop_Room()`이 `QProcess::terminate()`를 호출하는데, Windows에서는 이 요청이
    콘솔 프로그램에 전달되지 않는다. 그래서 v6~v7 서버가 graceful shutdown 없이 `kill()`로 끝났다(서버 종료 코드 62097).
  - **보완 내용 (2026-11-19):** Windows에서는 서버의 종료 방식(Enter 입력)에 맞춰 표준 입력으로 줄바꿈을 보내고
    쓰기 채널을 닫도록 했다. 3초 뒤 강제 종료는 응답이 없을 때의 안전장치로 남겼다. 같은 GUI 조작에서
    `pending I/O drained`, `server stopped` 로그와 서버 종료 코드 0을 확인했다.
