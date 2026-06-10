# queue_drain

> 🌐 언어: **한국어** | [English](README.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../../EXAMPLES.kr.md)

전형적인 "수신 스레드 -> 큐 -> 처리 스레드" 패턴을 uniflow로 옮긴 모습. `UF_Sender`가 1초 간격으로 1~10개 메시지를 큐에 쌓고, `UF_Receiver`가 큐가 빌 때까지 하나씩 빼서 처리합니다.

---

## 무엇을 보여주나

- 메시지 큐가 *락 없는* 이유: sender와 receiver가 같은 펌프 스레드 위
- sender가 메시지를 쌓고 `receiver.IsIdle()`이면 즉시 `UF_START_FLOW`로 깨움
- receiver의 dispatch 패턴 - `OnRecv_TakeNext` -> `OnRecv_Add` 또는 `OnRecv_Sub` -> 다시 `OnRecv_TakeNext`
- Win32 viz 패널: 두 소스 벡터, 현재 큐 내용, 수신부 상태, 마지막 결과

**현실 매핑** - 진짜 시스템에서는 송신부가 다른 머신/다른 프로세스에서 옵니다. uniflow는 *받은 다음 자기 안에서 어떻게 처리할지*의 골격을 줍니다. mutex-친화적인 inbox 하나 + receiver 모듈 하나로 진짜 시스템도 같은 모양이 됩니다.

---

## 보면 좋은 파일

- [uf_sender.cpp](uf_sender.cpp) - burst 생성 + receiver 깨우기
- [uf_receiver.cpp](uf_receiver.cpp) - drain 루프 + dispatch
- [uf_visualization.cpp](uf_visualization.cpp) - 칩 기반 Win32 패널

---

## 빌드 / 실행

Win32 시각화를 쓰므로 Windows + MSVC 환경입니다.

```powershell
# Visual Studio
cpp\examples\queue_drain\queue_drain.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I cpp cpp\examples\queue_drain\*.cpp /Fe:queue_drain.exe
```

---

## 더 읽기

- 모듈 간 통신과 IsIdle 폴링: [TUTORIAL.kr.md 챕터 5, 마지막 챕터](../../../TUTORIAL.kr.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../../EXAMPLES.kr.md)
