# message_dispatch

> Language: **한국어** | [English](README.md)

콘솔 예제: 학생(worker) 하나, 스포너(spawner) 둘(교수, 친구), 공유 메일박스 하나.
교수는 과제(assignment)를, 친구는 놀자(play) 메시지를 던지고, 학생은 메일박스를
한 번에 하나씩 비우면서 메시지 종류(kind)에 따라 분기 처리합니다. 네 모듈 모두
같은 Runtime에 붙어 있어, 메일박스는 단일 펌프 스레드에서만 만져지므로 락이
필요 없습니다.

```
  uniflow message_dispatch  v1.0.0
  professor + friend -> shared mailbox -> student   (one Runtime, one pump, lock-free)
  ----------------------------------------------------------------------
  Professor  2/4 sent   posted "project"  inbox=2
  Friend     1/3 sent   waiting before next invite
  ----------------------------------------------------------------------
  Mailbox  (2 queued)
    ASN  project    need_ability=8 need_time=9h
    PLAY soccer     6h
  ----------------------------------------------------------------------
  Student   active: ASN  lab        need_ability=5 need_time=6h
    phase: training (+1 ability, 3h)
    ability [######..............]  3/10
    stress  [####................]  2/10
    hours spent: 18        messages handled: 3
  ----------------------------------------------------------------------
  Recent activity
    student trained -> ability=3 stress=2
    professor posted assignment "project"  inbox=2
  ----------------------------------------------------------------------
  press Enter to quit
```

## 핵심 기능 (Feature focus)

이 예제의 주제는 **하나의 협력 펌프 위에서의 메시지 라우팅**입니다.

- **메시지 종류별 분기 (dispatch by kind).** 학생의 `Step1_TakeNext`가 `Message`
  하나를 꺼내 `Message::Kind`로 분기합니다. `Assignment`는 train/sleep/work
  체인으로 가고(`Step2_CheckAbility`가 능력/스트레스로 다음 step을 가름),
  `Play`는 play 체인으로 갑니다. (참고: [uf_student.cpp](uf_student.cpp))

- **단일 펌프 위의 락 없는 공유 메일박스.** 스포너 둘이 push하고 학생이 pop하지만
  모든 step이 같은 펌프 스레드에서 돌므로 [mailbox.cpp](mailbox.cpp)는 뮤텍스
  없는 평범한 `std::deque`입니다. 이것이 uniflow의 핵심 보장입니다: 한 스레드,
  협력 모듈 사이에 락 없음.

- **SubmitAsync + 폴링으로 블로킹 작업 처리.** "n 시뮬레이션 시간" 작업을 인라인
  으로 돌리면 펌프가 멈추므로, 각 체인 step은 `SubmitAsync(UF_FN(SimHours), ...)`
  로 풀에 떠넘기고, 반환된 `AsyncId`를 `Next(UF_FN(StepX_Wait), job)`으로 대기
  step에 넘긴 뒤, 거기서 `AsyncResult<int>(job)`를 폴링합니다 -- `pending()`이면
  `Stay()`, 끝나면 `*r.return_value`를 읽습니다. 학생이 "일하는" 동안에도 펌프는
  스포너들을 계속 굴립니다.

- **콘솔 / ANSI 렌더 패턴.** `Flow_Visualization` 태스크가 매 펌프 라운드마다
  세계 상태를 `g_snap`으로 스냅샷하고, 백그라운드 렌더 스레드가 그 스냅샷으로
  약 25fps로 대시보드를 그립니다. 메인 스레드는 `std::getline`에서 블로킹
  합니다(Enter로 종료). 렌더러가 stdout을 소유하므로 Runtime은 `SilentObserver`로
  step 트레이스 출력을 막습니다. [console.h](console.h) / [console.cpp](console.cpp)
  는 simulator 및 city_traffic / pick_and_place 콘솔 렌더러와 동일한 의존성 없는
  ANSI 헬퍼입니다.

## 파일 맵 (File map)

| 파일 | 역할 |
| --- | --- |
| [uf_student.h](uf_student.h) / [.cpp](uf_student.cpp) | worker. `Task_Drain` 하나가 메시지를 꺼내 종류별로 분기, 체인 실행, SimHours를 SubmitAsync로 떠넘김. |
| [uf_professor.h](uf_professor.h) / [.cpp](uf_professor.cpp) | 스포너: 무작위 간격으로 과제 post, 학생을 깨움. |
| [uf_friend.h](uf_friend.h) / [.cpp](uf_friend.cpp) | 스포너: 놀자 invite post. 교수와 같은 구조. |
| [mailbox.h](mailbox.h) / [.cpp](mailbox.cpp) | 공유 inbox (락 없음; 펌프 스레드 전용). |
| [globals.h](globals.h) / [.cpp](globals.cpp) | Message 타입, 타이밍 config, 종료 latch, 최근 활동 로그. |
| [uf_visualization.h](uf_visualization.h) / [.cpp](uf_visualization.cpp) | 펌프 측 스냅샷 태스크 + 백그라운드 ANSI 렌더 루프. |
| [snapshot.h](snapshot.h) / [.cpp](snapshot.cpp) | 펌프 -> 렌더 스레드 전달 (cross-thread라 뮤텍스 사용). |
| [console.h](console.h) / [.cpp](console.cpp) | 재사용 ANSI 콘솔 헬퍼. |
| [app.h](app.h) | Runtime + 모든 모듈, 2단계 초기화, silent observer, `friend` 키워드 충돌 회피(`friend_`). |
| [main.cpp](main.cpp) | ANSI 활성화, 앱 시작, 렌더 스레드 구동, Enter 블로킹, 종료. |

## 빌드 / 실행

콘솔 전용, 추가 설치 없음.

**Linux / macOS (g++ 또는 clang++):**

```sh
cd cpp/examples/message_dispatch
g++ -std=c++17 -O2 -I../.. *.cpp -o message_dispatch -pthread
./message_dispatch
```

**Windows (MSVC, x64 Native Tools 프롬프트):**

```bat
cd cpp\examples\message_dispatch
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:message_dispatch.exe
message_dispatch.exe
```

> 참고: 이 데모는 ANSI 터미널을 가정합니다. Windows에서는 `console::EnableAnsi()`가
> 콘솔의 VT 처리를 켭니다(Windows Terminal 권장).
