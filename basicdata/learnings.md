# 깨달은 점 (버전별 학습 노트)

버그를 고치거나 개념을 짚으면서 실제로 이해하게 된 포인트를 남긴다. 버그 자체의 재현/수정 절차는 [troubleshooting.md](troubleshooting.md)에, 여기는 "왜 그런지"에 대한 개념 정리 위주.

## v1 — 싱글 스레드 blocking TCP 서버

- **POSIX API는 예외(exception)가 아니라 리턴값으로 에러를 알려준다.** `socket()`/`bind()`/`listen()`/`accept()` 등을 `try/catch`로 감싸는 건 의미가 없음 — 실패하면 음수를 리턴할 뿐 예외를 던지지 않는다. 항상 반환값을 직접 `if`로 체크해야 함.

- **함수의 리턴값이 항상 "저장할 가치가 있는 데이터"는 아니다.** `socket()`은 성공 시 실제로 쓸 fd를 리턴하지만, `bind()`/`listen()`은 성공/실패(0/음수)만 알려줄 뿐이라 그 값을 멤버 변수에 대입하면 오히려 기존 값을 덮어쓰는 버그가 됨

- **리스닝 소켓과 클라이언트 소켓은 서로 다른 fd이고 생명주기도 다르다.** 클라이언트 연결을 정리할 때 실수로 리스닝 소켓(`this->fd`)을 닫으면, 그 이후 `accept()`가 전부 실패해서 사실상 서버가 죽어버림. 리스닝 소켓은 서버 프로세스가 끝날 때(소멸자)만 닫아야 하고, 클라이언트 소켓은 그 클라이언트와의 연결이 끝날 때마다 닫아야 함.

- **`accept()`의 클라이언트 주소 구조체는 input이 아니라 output이다.** `bind()`에 쓰는 주소 구조체(내가 원하는 바인딩 주소를 알려주는 input)와 헷갈리기 쉬운데, `accept()`의 구조체는 커널이 "누가 접속했는지"를 채워주는 output이라 호출 전에 미리 값을 넣어봤자 의미가 없음.

- **`accept()`/`recv()`는 진짜로 스레드를 멈춘다(blocking).** CPU를 쓰면서 반복 확인(polling)하는 게 아니라, 커널이 "할 일이 생길 때까지 이 스레드는 깨우지 마"라고 완전히 재워버림. 그래서 한 클라이언트를 처리(특히 `recv()`에서 대기)하는 동안 다른 클라이언트의 접속 요청은 커널의 backlog 큐에 쌓이기만 하고 처리되지 않음 — 두 개의 `nc`를 동시에 열어서 직접 확인함(첫 연결 종료 후에야 두 번째가 응답받음).

- **자원(fd)을 직접 들고 있는 클래스는 복사(copy)에 취약하다 (Rule of Three).** 지금은 문제가 되지 않지만, 객체가 복사되면 같은 fd를 두 객체가 각자 `close()`하려는 이중 close 버그로 이어질 수 있음 — v2에서 스레드로 객체를 넘기게 될 때 다시 짚기로 함.

## v2 — thread-per-connection

- **비정적 멤버 함수의 주소는 반드시 `&ClassName::MemberFunc`처럼 스코프를 명시해야 한다.** 멤버 함수 본문 안에서는 다른 멤버 함수 이름을 그냥 써도 호출은 되지만, `std::thread`에 넘기듯 그 "주소"를 값으로 만들려는 순간에는 이름만으로 부족하다 — 컴파일러가 어떤 클래스의 멤버인지 판단할 근거가 없기 때문. `thread t(Process_Client, this, fd)`가 아니라 `thread t(&ThreadPerConnectionEchoServer::Process_Client, this, fd)`로 써야 함.

- **`recv()`로 채운 버퍼는 널 종료가 보장되지 않는다.** 문자열처럼 로그에 찍고 싶으면 `std::string(buffer, recv_result)`처럼 유효한 길이를 명시해서 감싸야 한다. 그냥 `cout << buffer`로 찍으면 이전 반복의 잔여 데이터나 초기화되지 않은 값 때문에 쓰레기 문자가 더 찍히거나, 최악의 경우 배열 범위를 넘어 읽는 UB로 이어질 수 있음.

- **`detach()`는 스레드 수명 관리를 포기하는 대신 구현을 단순하게 만드는 트레이드오프다.** thread-per-connection처럼 스레드가 일회성이면 detach로 충분하지만(스레드가 알아서 끝남), v3의 고정 크기 thread pool처럼 스레드를 미리 만들어 재사용하는 구조에서는 서버 종료 시 각 워커를 안전하게 멈추고 `join()`으로 정리하는 과정이 반드시 필요해진다.

## v3 — 고정 크기 thread pool + bounded 작업 큐

- **`condition_variable::wait()`는 mutex를 계속 잡고 잠드는 것이 아니다.** 대기하는 동안
  mutex를 풀어 생산자가 큐에 작업을 넣을 수 있게 하고, 깨어난 뒤 mutex를 다시 획득한
  상태에서 predicate를 확인한다. 그래서 작업 큐의 확인과 pop을 하나의 임계 영역으로
  유지할 수 있다.

- **중첩된 반복문이 많다고 시간복잡도가 자동으로 커지는 것은 아니다.** worker의 바깥
  반복문은 다음 작업을 기다리는 수명 주기이고, 안쪽 `recv()` 반복문은 현재 연결 하나의
  메시지를 처리한다. 두 반복의 입력 크기를 곱해 전부 순회하는 구조가 아니므로 코드의
  중첩 깊이만 보고 `O(n²)`로 판단할 수 없다.

- **고정 thread pool은 스레드 수를 제한하지만 대기 작업 수까지 제한하지는 않는다.**
  별도의 최대 큐 크기가 없으면 worker보다 빠르게 연결이 들어올 때 fd가 계속 누적된다.
  v3에서는 큐를 128개로 제한하고 초과 연결을 닫는 정책을 적용했다.

- **종료 상태만 바꿔서는 blocking 시스템 호출이 자동으로 끝나지 않는다.** worker의
  `recv()`는 client 소켓 `shutdown()`으로 깨웠고, macOS에서 listen 소켓 `shutdown()`만으로
  `accept()`가 풀리지 않는 경우에는 non-blocking listen fd와 timeout이 있는 `poll()`로
  종료 상태를 다시 확인하도록 구성했다.

- **주기 작업도 종료 알림을 받을 수 있어야 한다.** monitor가 `sleep_for(30초)`를 사용하면
  종료 직후에도 최대 30초 동안 join을 기다린다. 같은 condition variable의 `wait_for()`와
  종료 predicate를 사용하면 평상시 주기는 유지하면서 `notify_all()`에는 즉시 반응한다.

- **signal handler에서 일반 C++ 동기화 코드를 직접 호출하면 안 된다.** mutex와
  condition variable 조작은 async-signal-safe하지 않으므로 SIGINT/SIGTERM을 막은 뒤
  main 스레드의 `sigwait()`에서 동기적으로 받아 `Stop()`을 호출했다.

## v4 — Linux epoll 기반 단일 스레드 reactor

- **fd는 현재 프로세스 안에서 열린 자원을 가리키는 번호이며, client fd는 임시 연결 id처럼
  사용할 수 있다.** listen fd는 새 접속을 받는 문지기 소켓이고, client fd는 이미 연결된
  클라이언트와 실제로 `recv()`/`send()`하는 소켓이다. 그래서 `event_fd == listen_fd`이면
  `accept()`, 그 외 client fd이면 `recv()`로 분기한다.

- **`epoll_wait()`는 “이번에 준비된 fd 목록”을 돌려준다.** listen fd에 새 접속이 남아 있는데
  `accept()`로 소비하지 않으면 같은 listen 이벤트가 계속 발생하고, client fd에 읽을 데이터가
  남아 있는데 `recv()`로 소비하지 않으면 같은 client 이벤트가 반복된다.

- **`epoll_ctl()`의 첫 번째 인자는 감시자인 `epoll_fd`, 세 번째 인자는 감시 대상 fd다.**
  client fd를 등록할 때 `epoll_ctl(client_fd, ...)`처럼 쓰면 `Invalid argument`가 발생한다.
  올바른 의미는 `this->epoll_fd`의 감시 목록에 `client_fd`를 추가하는 것이다.

- **`eventfd`는 `epoll_wait()`를 깨우기 위한 fd로 사용할 수 있다.** `stopping` 같은 메모리
  변수만 바꿔서는 커널 안에서 잠든 `epoll_wait()`를 즉시 깨우지 못한다. `Stop()`이
  `stop_event_fd`에 값을 write하면 epoll이 stop 이벤트를 돌려주고, 이벤트 루프는 값을
  read한 뒤 종료한다.

- **정리 함수는 여러 번 호출되어도 안전해야 한다.** `Server_Run()` 종료와 소멸자 경로에서
  cleanup이 중복 호출될 수 있으므로 `cleaned_up` 플래그로 fd close와 로그가 한 번만
  수행되도록 했다.

- **Docker 컨테이너의 localhost와 macOS의 localhost는 다르다.** 컨테이너 안의 서버에
  macOS 터미널에서 접속하려면 `docker run -p 9000:9000 ...`처럼 포트 매핑이 필요하다.
