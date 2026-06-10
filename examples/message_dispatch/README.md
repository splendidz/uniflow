# message_dispatch

> 🌐 언어: **한국어** | [English](README.en.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../EXAMPLES.md)

세 모듈(`UF_Professor`, `UF_Friend`, `UF_Student`)이 공유 메일박스로 협력합니다. 교수는 과제를, 친구는 놀자는 메시지를 던지고, 학생은 받은 순서로 처리합니다. 학생은 능력이 부족하면 훈련, 너무 지치면 잠, 충분하면 과제 수행 - 놀이는 별도 chain입니다.

---

## 무엇을 보여주나

- *세 모듈이 동시에 도는 협력 모델*. 모듈 사이 통신은 락 없는 메일박스 + `IsIdle` 폴링
- 학생의 한 flow 안에서 *조건부 chain* - `OnAssign_CheckAbility`가 다음 step을 분기
- `UF_ASYNC(SimHours, n)`으로 "n시간짜리 작업" 시뮬레이션. 학생이 *일하는 동안* 펌프는 다른 모듈을 돌립니다
- Win32 viz: 칩으로 표현한 메일박스, 능력/스트레스 게이지 바, 양쪽 스폰서의 pending 리스트

---

## 보면 좋은 파일

- [uf_student.cpp](uf_student.cpp) - train / sleep / work / play 분기
- [uf_visualization.cpp](uf_visualization.cpp) - 깔끔한 패널 + 게이지 바
- [app.h](app.h) - `friend` 키워드 충돌 회피 (`friend_` 멤버명)

---

## 빌드 / 실행

Win32 시각화를 쓰므로 Windows + MSVC 환경입니다.

```powershell
# Visual Studio
examples\message_dispatch\message_dispatch.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\message_dispatch\*.cpp /Fe:message_dispatch.exe
```

---

## 더 읽기

- 한 flow 안의 조건부 분기와 async: [TUTORIAL.md 챕터 2, 6](../../TUTORIAL.md)
- 모듈 간 통신: [TUTORIAL.md 챕터 5, 마지막 챕터](../../TUTORIAL.md)
- 전체 예제 갤러리: [EXAMPLES.md](../../EXAMPLES.md)
