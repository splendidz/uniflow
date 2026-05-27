# uniflow 예제 모음

> 🌐 언어: **한국어** · [English](EXAMPLES.en.md)

다섯 개의 동작하는 프로젝트. 모두 빌드하면 바로 도는 *완성된* 코드입니다.

| # | 이름 | 한 줄 | 난이도 | UI |
|---|---|---|---|---|
| 0 | [cnc_pickers](#0-cnc_pickers--레퍼런스-프로젝트) | CNC 라인 풀 시뮬레이션 | ★★★★☆ | Win32 |
| 1 | [shared_ostream](#1-shared_ostream--lock-free-공유-자원) | 모듈 둘이 한 ostringstream을 락 없이 공유 | ★☆☆☆☆ | 콘솔 |
| 2 | [queue_drain](#2-queue_drain--보내고-받고-한꺼번에-드레인) | 송수신 큐 drain 패턴 | ★★☆☆☆ | Win32 |
| 3 | [message_dispatch](#3-message_dispatch--교수-친구-학생) | 두 스폰서 → 학생 메시지 디스패치 | ★★★☆☆ | Win32 |
| 4 | [weather_llm](#4-weather_llm--기상청--gemini) | 기상청 XML → Gemini 요약 | ★★★★★ | 콘솔 |

모든 예제의 빌드 / 실행은 같은 패턴입니다:

```powershell
# Visual Studio
examples\<이름>\<이름>.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\<이름>\*.cpp /Fe:<이름>.exe
```

---

## 0. cnc_pickers — 레퍼런스 프로젝트

![cnc_pickers demo](docs/videos/cnc_pickers.gif)

> 📹 동영상 자리: `docs/videos/cnc_pickers.mp4`

세 모듈 (`UF_LoadPicker`, `UF_Stage`, `UF_UnloadPicker`) 과 오케스트레이터 한 개로 구성된 가상 CNC 가공 라인. 픽커 둘이 zone B 에 동시에 들어가지 않게 하면서 라인을 연속 운전합니다.

**무엇을 보여주나**
- 모터/그리퍼 모션의 *Cmd → Wait* 2-step 패턴 (`OnLoad_CmdMoveToSource` → `OnLoad_WaitAtSource`)
- 가상 하드웨어 (`MotorAxis`) 의 settle 시간까지 모델링한 폴링
- 오케스트레이터가 `picker.IsIdle()` / `stage.state()` 를 보고 다음 행동 결정
- `UF_Visualization` 모듈 + Win32 메시지 루프로 라인 실시간 시각화
- `EnvLogObserver` 가 콘솔과 로그 파일에 동시 출력

**보면 좋은 파일**
- [uf_orchestrator.cpp](examples/cnc_pickers/uf_orchestrator.cpp) — 스케줄링 결정의 모든 것
- [uf_load_picker.cpp](examples/cnc_pickers/uf_load_picker.cpp) — 16 step짜리 픽커 사이클
- [app.h](examples/cnc_pickers/app.h) — 모듈 두 단계 초기화 패턴

---

## 1. shared_ostream — lock-free 공유 자원

![shared_ostream demo](docs/videos/shared_ostream.gif)

> 📹 동영상 자리: `docs/videos/shared_ostream.mp4`

가장 작은 예제. 같은 Runtime에 매단 모듈 두 개가 한 `std::ostringstream` 에 락 없이 번갈아 적습니다.

**무엇을 보여주나**
- `UF_Writer` 인스턴스 둘 (`"Hello"`, `"World"`) 이 같은 펌프 스레드 위에서 도는 사실 자체
- 공유 turn 플래그로 순서 강제 (`SharedState::Turn()`)
- 출력 검증: `"Hello World."` 가 정확히 10회 등장하는지 확인

**왜 중요한가** — uniflow의 가장 본질적 성질: *같은 Runtime의 모든 step body는 한 스레드*. 모듈이 둘이든 열이든, 공유 자원에 mutex 안 써도 race 없음. 이 예제가 그걸 *증명* 하는 코드입니다.

**실행 결과**
```
=== shared_ostream: two writers, one log, no locks ===
...
--- output ---
Hello World. Hello World. Hello World. Hello World. Hello World.
Hello World. Hello World. Hello World. Hello World. Hello World.
--- end ---

expected "Hello World." occurrences = 10, got = 10
PASS: shared log is race-free, order preserved
```

**보면 좋은 파일**
- [uf_writer.cpp](examples/shared_ostream/uf_writer.cpp) — 16줄짜리 핵심
- [main.cpp](examples/shared_ostream/main.cpp) — 검증 로직

---

## 2. queue_drain — 보내고, 받고, 한꺼번에 드레인

![queue_drain demo](docs/videos/queue_drain.gif)

> 📹 동영상 자리: `docs/videos/queue_drain.mp4`

전형적인 "수신 스레드 → 큐 → 처리 스레드" 패턴을 uniflow로 옮긴 모습. `UF_Sender` 가 1초 간격으로 1~10개 메시지를 큐에 쌓고, `UF_Receiver` 가 큐가 빌 때까지 하나씩 빼서 처리합니다.

**무엇을 보여주나**
- 메시지 큐가 *락 없는* 이유: sender와 receiver가 같은 펌프 스레드 위
- sender가 메시지 쌓고 `receiver.IsIdle()` 이면 즉시 `UF_START_FLOW` 로 깨움
- receiver의 dispatch 패턴 — `OnRecv_TakeNext` → `OnRecv_Add` 또는 `OnRecv_Sub` → 다시 `OnRecv_TakeNext`
- Win32 viz 패널: 두 소스 벡터, 현재 큐 내용, 수신부 상태, 마지막 결과

**현실 매핑** — 진짜 시스템에서는 송신부가 다른 머신/다른 프로세스에서 옵니다. uniflow는 *받은 다음 자기 안에서 어떻게 처리할지* 의 골격을 줍니다. mutex-친화적인 inbox 한 개 + receiver 모듈 한 개로 진짜 시스템도 같은 모양이 됩니다.

**보면 좋은 파일**
- [uf_sender.cpp](examples/queue_drain/uf_sender.cpp) — burst 생성 + receiver 깨우기
- [uf_receiver.cpp](examples/queue_drain/uf_receiver.cpp) — drain 루프 + dispatch
- [uf_visualization.cpp](examples/queue_drain/uf_visualization.cpp) — 칩 기반 Win32 패널

---

## 3. message_dispatch — 교수, 친구, 학생

![message_dispatch demo](docs/videos/message_dispatch.gif)

> 📹 동영상 자리: `docs/videos/message_dispatch.mp4`

세 모듈(`UF_Professor`, `UF_Friend`, `UF_Student`)이 공유 메일박스로 협력. 교수는 과제를, 친구는 놀자고 메시지를 던지고, 학생은 받은 순서로 처리합니다. 학생은 능력이 부족하면 `훈련`, 너무 지치면 `잠`, 충분하면 `과제 수행` — `놀이`는 별도 chain.

**무엇을 보여주나**
- *세 모듈이 동시에 도는 협력 모델*. 모듈 사이 통신은 락 없는 메일박스 + `IsIdle` 폴링
- 학생의 한 flow 안에서 *조건부 chain* — `OnAssign_CheckAbility` 가 다음 step을 분기
- `UF_ASYNC(SimHours, n)` 으로 "n시간짜리 작업" 시뮬레이션. 학생이 *일하는 동안* 펌프는 다른 모듈을 돌립니다
- Win32 viz: 칩으로 표현한 메일박스, 능력/스트레스 게이지 바, 양쪽 스폰서의 pending 리스트

**보면 좋은 파일**
- [uf_student.cpp](examples/message_dispatch/uf_student.cpp) — train / sleep / work / play 분기
- [uf_visualization.cpp](examples/message_dispatch/uf_visualization.cpp) — 깔끔한 패널 + 게이지 바
- [app.h](examples/message_dispatch/app.h) — `friend` 키워드 충돌 회피 (`friend_` 멤버명)

---

## 4. weather_llm — 기상청 + Gemini

![weather_llm demo](docs/videos/weather_llm.gif)

> 📹 동영상 자리: `docs/videos/weather_llm.mp4`

진짜 비동기. 모듈 하나(`UF_Weather`)가 세 step에 걸쳐 *바깥 세계* 와 통신합니다:

1. `UF_ASYNC`: 기상청 DFS 예보 XML을 WinHTTP로 HTTPS GET
2. `UF_ASYNC`: 받은 XML을 Google Gemini `generateContent` 엔드포인트에 POST
3. Gemini가 돌려준 한국어 요약을 콘솔에 출력

**무엇을 보여주나**
- 한 모듈의 한 flow에서 *두 번의 async*. 첫 결과를 둘째 호출에 인자로 넘김
- step body는 *전혀* 블로킹하지 않음 — 펌프 스레드는 GET 동안에도, POST 동안에도 자유
- 실패/타임아웃 처리. `AsyncResult<T>::failed()` 와 `is_timeout()`
- UTF-8 콘솔 모드 세팅 (`SetConsoleOutputCP(CP_UTF8)`) — 한국어 응답 정상 출력
- 환경변수로 API 키 주입 (`GEMINI_API_KEY`)

**실행 예시**
```powershell
$env:GEMINI_API_KEY = "AIza...<key from aistudio.google.com>"
weather_llm.exe
```
```
=== weather_llm: KMA front page -> Gemini summary ===
[weather] fetching https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127
[weather] got 6698 bytes of XML
[weather] submitting POST to Gemini (gemini-2.5-flash, prompt=...)

=== Gemini summary ===
  location: 서울 (그리드 60, 127)
  temperature: 21.0도 C
  sky/precipitation: 비
  humidity: 85%
  wind: 동풍 3.1m/s
  air_quality (PM10/PM2.5): <not found - 이 피드는 미세먼지 데이터 미포함>
=== end ===
```

**보면 좋은 파일**
- [uf_weather.cpp](examples/weather_llm/uf_weather.cpp) — 3단 async chain
- [http_client.cpp](examples/weather_llm/http_client.cpp) — WinHTTP 래퍼 + JSON escape + 응답 파서
- [main.cpp](examples/weather_llm/main.cpp) — UTF-8 콘솔 설정

**API 키 받기** — https://aistudio.google.com/app/apikey — 구글 계정으로 무료 발급, free tier 한도 안에서 동작.

---

## 어떤 순서로 보면 좋은가

처음 시작이면 **튜토리얼** 챕터 1~5 → **shared_ostream** → **queue_drain** → **message_dispatch** → **weather_llm** → **cnc_pickers** 순서를 권합니다.

처음부터 *전체 모양* 을 보고 싶다면 **cnc_pickers** 먼저 훑고 튜토리얼로 돌아가도 좋습니다.

---

## 동영상 추가 위치

데모 영상은 [docs/videos/](docs/videos/) 폴더에 모입니다. 권장 형식:

- 길이: 20~40초
- 형식: MP4 (Github 미리보기) 또는 GIF (작은 용량)
- 파일명: `<예제이름>.mp4` 또는 `<예제이름>.gif`
- 해상도: 720p~1080p, 30fps

각 섹션의 `📹 동영상 자리:` 라인 위 `![]()` 한 줄이 미리 준비돼 있으니 파일만 두면 바로 표시됩니다.
