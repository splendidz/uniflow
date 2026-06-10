# weather_llm

> 🌐 언어: **한국어** | [English](README.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../../EXAMPLES.kr.md)

진짜 비동기 I/O. 모듈 하나(`UF_Weather`)가 세 step에 걸쳐 *바깥 세계*와 통신합니다:

1. `UF_ASYNC`: 기상청 DFS 예보 XML을 WinHTTP로 HTTPS GET
2. `UF_ASYNC`: 받은 XML을 Google Gemini `generateContent` 엔드포인트에 POST
3. Gemini가 돌려준 한국어 요약을 콘솔에 출력

---

## 무엇을 보여주나

- 한 모듈의 한 flow에서 *두 번의 async*. 첫 결과를 둘째 호출에 인자로 넘김
- step body는 *전혀* 블로킹하지 않음 - 펌프 스레드는 GET 동안에도, POST 동안에도 자유
- 실패/타임아웃 처리. `AsyncResult<T>::failed()`와 `is_timeout()`
- UTF-8 콘솔 모드 세팅 (`SetConsoleOutputCP(CP_UTF8)`) - 한국어 응답 정상 출력
- 환경변수로 API 키 주입 (`GEMINI_API_KEY`)

---

## 보면 좋은 파일

- [uf_weather.cpp](uf_weather.cpp) - 3단 async chain
- [http_client.cpp](http_client.cpp) - WinHTTP 래퍼 + JSON escape + 응답 파서
- [main.cpp](main.cpp) - UTF-8 콘솔 설정

---

## 빌드 / 실행

WinHTTP를 쓰므로 Windows + MSVC 환경입니다. Google AI Studio에서 무료 API 키를 발급받아 환경변수로 넣습니다.

```powershell
# Visual Studio
cpp\examples\weather_llm\weather_llm.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I cpp cpp\examples\weather_llm\*.cpp /Fe:weather_llm.exe

$env:GEMINI_API_KEY = "AIza...<key from aistudio.google.com>"
weather_llm.exe
```

실행 예시:

```text
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

**API 키 받기** - https://aistudio.google.com/app/apikey - 구글 계정으로 무료 발급, free tier 한도 안에서 동작.

---

## 더 읽기

- UF_ASYNC와 결과 수신, 타임아웃/에러: [TUTORIAL.kr.md 챕터 6-7](../../../TUTORIAL.kr.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../../EXAMPLES.kr.md)
