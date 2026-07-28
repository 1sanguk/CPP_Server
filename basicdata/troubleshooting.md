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

### [v2] 멤버 함수 포인터 스코프 누락으로 컴파일 실패
- 증상: `thread t(Process_Client, this, accept_result);`처럼 클래스 스코프 없이 멤버 함수 이름만 넘기면 컴파일 에러가 발생함.
- 원인: 비정적 멤버 함수의 주소를 얻으려면 반드시 `&ClassName::MemberFunc` 형태로 명시적으로 스코프를 붙여야 함. 이름만 쓰면 컴파일러가 어떤 클래스의 멤버인지 알 수 없어 포인터-투-멤버 타입을 만들지 못함.
- 재현 방법: 위 코드를 그대로 작성 후 빌드해서 에러 확인.
- 해결: `thread t(&ThreadPerConnectionEchoServer::Process_Client, this, accept_result);`로 수정.
- 배운 점: 멤버 함수를 콜백/스레드 진입점으로 넘길 때는 항상 `&Class::Member` 형태를 써야 한다.

### [v3] listen 소켓 shutdown 후에도 accept가 끝나지 않음

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

### [v3] non-blocking listen 설정 후 client recv가 EAGAIN으로 종료됨

- 증상: 연결 직후 worker의 `recv()`가 `Resource temporarily unavailable`을 출력하고
  client 연결을 닫았다.
- 원인: 로컬 환경에서 listen fd의 `O_NONBLOCK` 상태가 accept된 client fd에도 이어졌지만,
  v3의 `Process_Client()`는 blocking 소켓을 전제로 `EAGAIN`을 처리하지 않았다.
- 재현 방법: non-blocking listen fd로 서버를 실행하고 장기 연결 6개를 생성했다.
- 해결: accept 직후 client fd의 `O_NONBLOCK` 플래그를 제거해 v3의 blocking worker
  구조를 유지했다.
- 배운 점: listener와 accepted socket의 플래그 상태를 플랫폼에서 당연히 분리해줄 것이라고
  가정하지 말고, 서버가 원하는 client socket 모드를 명시적으로 설정해야 한다.
