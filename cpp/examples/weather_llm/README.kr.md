# weather_llm

> Language: Korean | [English](README.md)  |  [Example gallery](../../EXAMPLES.kr.md)

실제 비동기 I/O. 모듈 하나(`Flow_Weather`)가 두 개의 연쇄 async 단계로
외부 서비스와 통신한 뒤 결과를 출력합니다:

1. 기상청 DFS 예보 XML을 WinHTTP로 HTTPS GET (풀에서 실행).
2. 받은 XML을 Google Gemini `generateContent` 엔드포인트에 POST (풀에서 실행).
3. Gemini가 돌려준 한국어 요약을 콘솔에 출력.

이 예제는 C++ 전용이며 Windows 지향입니다 (WinHTTP + 콘솔 사용).

---

## Feature focus

- `SubmitAsync` + poll 로 연쇄되는 두 async 단계. 각 블로킹 호출
  (`FetchHtml`, 그 다음 `CallGemini`)을 `SubmitAsync(UF_FN(fn),
  Duration::max(), args...)` 로 풀에 오프로드하면 `AsyncId` 가 반환됩니다.
  그 id를 `Next(UF_FN(StepN), id)` 로 다음 step에 넘기고, 그 step에서
  `AsyncResult<std::string>(id)` 로 결과를 폴링합니다: `r.pending()` ->
  `Stay()`, `r.ok()` -> `*r.return_value` 읽기.
- 펌프는 네트워크 I/O 에 절대 블로킹하지 않습니다. GET 도 POST 도 Runtime
  스레드 풀에서 돌고, 펌프 스레드는 그 동안에도 계속 돌며 다른 모든 flow에
  반응합니다.
- 환경변수 키로 선택적 LLM. `GEMINI_API_KEY` 를 `std::getenv` 로 읽습니다.
  키가 없으면 flow는 받아온 XML의 일부만 출력하고 정상 종료합니다 -
  Gemini 호출 없음, 크래시 없음.

---

## File map

- `uf_weather.h` / `uf_weather.cpp` - `Flow_Weather` 와 단일 `Task_Fetch`,
  세 step `Step1_Fetch` -> `Step2_AfterFetch` -> `Step3_AfterLlm`.
- `http_client.h` / `http_client.cpp` - WinHTTP 래퍼 (GET + Gemini POST),
  JSON escaper, 관대한 Gemini 응답/에러 파서.
- `app.h` - `Runtime` 와 모듈 하나; `Start()` 는
  `weather.ctx_fetch_.StartFlow()` 로 태스크를 띄웁니다.
- `main.cpp` - UTF-8 콘솔 설정 (`SetConsoleOutputCP(CP_UTF8)`) 과
  start/shutdown 브래킷.

---

## 빌드 / 실행

WinHTTP를 쓰므로 Windows + MSVC 예제이며 `winhttp.lib` 를 링크합니다. Google
AI Studio 에서 무료 API 키를 발급받아 환경변수로 넣습니다 (선택 사항 - 없으면
원본 XML을 출력하고 종료합니다).

```powershell
# Visual Studio
# cpp\examples\weather_llm\weather_llm.vcxproj 를 솔루션에 추가하고 F5

# CLI (MSVC) - vcvars64.bat 실행 후 cpp\examples\weather_llm 안에서
cl /std:c++17 /EHsc /W4 /nologo /I..\.. main.cpp http_client.cpp uf_weather.cpp /Fe:weather_llm.exe winhttp.lib

$env:GEMINI_API_KEY = "AIza...<key from aistudio.google.com>"   # 선택
.\weather_llm.exe
```

실행 예시 (키 설정 시):

```text
=== weather_llm: KMA front page -> Gemini summary ===
[weather] fetching https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127
[weather] got 5998 bytes of XML
[weather] submitting POST to Gemini (gemini-2.5-flash, prompt=...)

=== Gemini summary ===
  location: 서울 (그리드 60, 127)
  temperature: 25.0도 C
  sky/precipitation: 소나기
  humidity: 70%
  wind: 남서풍 0.6m/s
=== end ===
```

`GEMINI_API_KEY` 가 없으면 XML을 받아 앞 500 바이트만 출력하고 정상
종료합니다.

API 키 받기: https://aistudio.google.com/app/apikey - 구글 계정으로 발급,
free tier 한도 안에서 동작.

---

## 더 읽기

- SubmitAsync, 결과 폴링, 타임아웃/에러: [TUTORIAL.kr.md](../../TUTORIAL.kr.md)
- 정식 Task/Step/async 레퍼런스: `../pick_and_place/uf_stage.cpp`
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../EXAMPLES.kr.md)

