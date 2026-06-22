# uniflow 예제 갤러리

> 언어: **한국어** | [English](EXAMPLES.md)

여섯 개의 레퍼런스 예제를 **C++ / Python / C# 세 언어로 동일하게** 제공한다(weather_llm 제외).
각 예제는 자기 폴더에 상세 설명 페이지를 두고, uniflow의 특정 기능을 다룬다.

| # | 예제 | 포커스 | 렌더링 | Python | C# |
|---|---|---|---|---|---|
| 1 | [pick_and_place](examples/pick_and_place/README.kr.md) | Task<Flow> 단위 + 오케스트레이터 상태 폴링 + async 폴링 ack (레퍼런스) | 듀얼 | [py](../python/examples/pick_and_place.py) | [cs](../cs/examples/pick_and_place/) |
| 2 | [city_traffic](examples/city_traffic/README.kr.md) | 수십 모듈 단일 스레드 협력 + 락 없는 공유 World | 듀얼 | [py](../python/examples/city_traffic.py) | [cs](../cs/examples/city_traffic/) |
| 3 | [simulator](examples/simulator/README.kr.md) | VirtualClock 가속/정지 + 렌더러도 flow + 락 없는 snapshot | 콘솔 | [py](../python/examples/simulator.py) | [cs](../cs/examples/simulator/) |
| 4 | [message_dispatch](examples/message_dispatch/README.kr.md) | 메시지 종류별 라우팅 + 락 없는 메일박스 + async 폴링 | 콘솔 | [py](../python/examples/message_dispatch.py) | [cs](../cs/examples/message_dispatch/) |
| 5 | [queue_drain](examples/queue_drain/README.kr.md) | 단일 스레드 생산자/소비자 + park/relaunch 웨이크 | 콘솔 | [py](../python/examples/queue_drain.py) | [cs](../cs/examples/queue_drain/) |
| 6 | [shared_ostream](examples/shared_ostream/README.kr.md) | 단일 펌프 = 공유 상태 락 프리 (최소 예제) | 콘솔 | [py](../python/examples/shared_ostream.py) | [cs](../cs/examples/shared_ostream/) |
| + | [weather_llm](examples/weather_llm/README.kr.md) | 두 단계 async 연쇄, 펌프는 네트워크에 블록 안 됨 (C++ 전용) | 콘솔 | - | - |

> "듀얼"은 Windows=Win32 GUI, Linux/macOS=ANSI 콘솔(`UF_RENDER=console`로 Windows에서도 콘솔)
> 이다. 나머지는 모두 콘솔이며 설치 없이 어디서나 실행된다.

빌드/실행:

```sh
# C++ 콘솔 예제 (Linux/macOS)
g++ -std=c++17 -O2 -I cpp cpp/examples/<이름>/*.cpp -o <이름> -pthread
# Python
python python/examples/<이름>.py
# C#
dotnet run --project cs/examples/<이름>
```

Windows에서는 듀얼 렌더 예제(pick_and_place / city_traffic)가 `.vcxproj`로 열리고,
콘솔 예제는 `cl /std:c++17 /EHsc /I cpp cpp\examples\<이름>\*.cpp /Fe:<이름>.exe`로 빌드한다.

---

## 어떤 순서로 보면 좋은가

처음이라면 [README](../README.kr.md)의 Quick Start -> [TUTORIAL.kr.md](TUTORIAL.kr.md) ->
**6 shared_ostream** -> **3 simulator** -> **5 queue_drain** -> **4 message_dispatch** ->
**2 city_traffic** -> **1 pick_and_place** 순서를 권한다.

전체 모양을 먼저 보려면 **city_traffic** 또는 **pick_and_place**를 먼저 훑은 뒤
튜토리얼로 돌아가면 된다.
