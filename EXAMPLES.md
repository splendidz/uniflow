# uniflow 예제 갤러리

> 🌐 언어: **한국어** | [English](EXAMPLES.en.md)

여섯 개의 동작하는 프로젝트. 모두 빌드하면 바로 도는 *완성된* 코드입니다. 각 예제는 자기 폴더에 상세 설명 페이지를 가지고 있습니다.

| 예제 | 한 줄 | 난이도 | UI |
|---|---|---|---|
| [city_traffic](examples/city_traffic/README.md) | 차량 수십 대가 단일 펌프로 도는 도시 주행 시뮬 (피날레) | ★★★★★ | Win32 |
| [cnc_pickers](examples/cnc_pickers/README.md) | CNC 라인 풀 시뮬레이션 (레퍼런스 프로젝트) | ★★★★☆ | Win32 |
| [shared_ostream](examples/shared_ostream/README.md) | 모듈 둘이 한 ostringstream을 락 없이 공유 | ★☆☆☆☆ | 콘솔 |
| [queue_drain](examples/queue_drain/README.md) | 송수신 큐 drain 패턴 | ★★☆☆☆ | Win32 |
| [message_dispatch](examples/message_dispatch/README.md) | 두 스폰서 -> 학생 메시지 디스패치 | ★★★☆☆ | Win32 |
| [weather_llm](examples/weather_llm/README.md) | 기상청 XML -> Gemini 요약 (실제 비동기 I/O) | ★★★★★ | 콘솔 |

모든 예제의 빌드/실행은 같은 패턴입니다:

```powershell
# Visual Studio
examples\<이름>\<이름>.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\<이름>\*.cpp /Fe:<이름>.exe
```

---

## 대표작

### [city_traffic](examples/city_traffic/README.md) - 마지막 피날레

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic demo" width="560"/>
</p>

수십 대의 차량이 각자 독립된 uniflow 모듈로, 공유 신호등과 앞차를 보며 가감속/정지/회전하며 도시를 순환합니다. 애플리케이션 레벨 스레드는 0개 - 모든 차량과 신호가 단일 펌프 위에서 돕니다. [상세 페이지](examples/city_traffic/README.md) | [폴더](examples/city_traffic/)

### [cnc_pickers](examples/cnc_pickers/README.md) - 레퍼런스 프로젝트

<p align="center">
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers demo" width="560"/>
</p>

세 모듈 + 오케스트레이터로 구성된 가상 CNC 가공 라인. 픽커 둘이 zone B에 동시에 들어가지 않게 하면서 라인을 연속 운전합니다. 모터/그리퍼 모션의 Cmd -> Wait 2-step 패턴이 핵심. [상세 페이지](examples/cnc_pickers/README.md) | [폴더](examples/cnc_pickers/)

---

## 나머지 예제

- [shared_ostream](examples/shared_ostream/README.md) - 가장 작은 예제. 같은 Runtime에 매단 모듈 둘이 한 `std::ostringstream`에 락 없이 번갈아 적습니다. uniflow의 가장 본질적 성질(한 Runtime의 모든 step은 한 스레드)을 *증명*하는 코드.
- [queue_drain](examples/queue_drain/README.md) - "수신 -> 큐 -> 처리" 패턴. Sender가 burst로 메시지를 쌓고, Receiver가 큐가 빌 때까지 drain. 큐가 락 없는 이유를 보여줍니다.
- [message_dispatch](examples/message_dispatch/README.md) - 교수/친구/학생 세 모듈이 공유 메일박스로 협력. 한 flow 안의 조건부 분기와 async 시뮬레이션.
- [weather_llm](examples/weather_llm/README.md) - 진짜 비동기 I/O. 기상청 XML을 HTTPS로 받아 Gemini로 요약. 한 flow에서 두 번의 async chain.

---

## 어떤 순서로 보면 좋은가

처음이라면 [README](README.md)의 3개 퀵 튜토리얼 -> [TUTORIAL.md](TUTORIAL.md) 챕터 1-5 -> **shared_ostream** -> **queue_drain** -> **message_dispatch** -> **weather_llm** -> **cnc_pickers** -> **city_traffic** 순서를 권합니다.

처음부터 *전체 모양*을 보고 싶다면 **city_traffic** 또는 **cnc_pickers**를 먼저 훑고 튜토리얼로 돌아가도 좋습니다.
