# weather_llm

> Language: [한국어](README.kr.md) | English  |  [Example gallery](../../EXAMPLES.md)

Real async I/O. A single module (`Flow_Weather`) communicates with external
services across two chained async stages, then prints the result:

1. HTTPS GET the KMA DFS forecast XML via WinHTTP (on the pool).
2. POST that XML to the Google Gemini `generateContent` endpoint (on the pool).
3. Print the Korean summary Gemini returns to the console.

This example is C++ only and Windows oriented: it uses WinHTTP and a console.

---

## Feature focus

- Two chained async stages via `SubmitAsync` + poll. Each blocking call
  (`FetchHtml`, then `CallGemini`) is offloaded with `SubmitAsync(UF_FN(fn),
  Duration::max(), args...)`, which returns an `AsyncId`. The id is carried to
  the next step with `Next(UF_FN(StepN), id)`, and that step polls the result
  with `AsyncResult<std::string>(id)`: `r.pending()` -> `Stay()`, `r.ok()` ->
  read `*r.return_value`.
- The pump never blocks on network I/O. Both the GET and the POST run on the
  Runtime thread pool; the pump thread keeps spinning and stays responsive to
  every other flow the whole time.
- Optional LLM via env key. `GEMINI_API_KEY` is read with `std::getenv`. If it
  is unset, the flow prints a slice of the fetched XML and exits cleanly - no
  network call to Gemini, no crash.

---

## File map

- `uf_weather.h` / `uf_weather.cpp` - `Flow_Weather` and its single `Task_Fetch`
  with the three steps `Step1_Fetch` -> `Step2_AfterFetch` -> `Step3_AfterLlm`.
- `http_client.h` / `http_client.cpp` - WinHTTP wrapper (GET + Gemini POST),
  a JSON escaper, and a forgiving Gemini response/error parser.
- `app.h` - the `Runtime` plus the one module; `Start()` launches the task with
  `weather.ctx_fetch_.StartFlow()`.
- `main.cpp` - UTF-8 console setup (`SetConsoleOutputCP(CP_UTF8)`) and the
  start/shutdown bracket.

---

## Build / run

WinHTTP makes this a Windows + MSVC example; it links `winhttp.lib`. Get a free
API key from Google AI Studio and pass it via an environment variable
(optional - without it the program prints raw XML and exits).

```powershell
# Visual Studio
# add cpp\examples\weather_llm\weather_llm.vcxproj to your solution and hit F5

# CLI (MSVC) - from inside cpp\examples\weather_llm after running vcvars64.bat
cl /std:c++17 /EHsc /W4 /nologo /I..\.. main.cpp http_client.cpp uf_weather.cpp /Fe:weather_llm.exe winhttp.lib

$env:GEMINI_API_KEY = "AIza...<key from aistudio.google.com>"   # optional
.\weather_llm.exe
```

Sample run (with a key set):

```text
=== weather_llm: KMA front page -> Gemini summary ===
[weather] fetching https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127
[weather] got 5998 bytes of XML
[weather] submitting POST to Gemini (gemini-2.5-flash, prompt=...)

=== Gemini summary ===
  location: Seoul (grid 60, 127)
  temperature: 25.0 C
  sky/precipitation: shower
  humidity: 70%
  wind: SW 0.6m/s
=== end ===
```

Without `GEMINI_API_KEY` set, the program fetches the XML and prints its first
500 bytes instead, then exits cleanly.

Get an API key: https://aistudio.google.com/app/apikey - available with a Google
account, works within the free tier.

---

## Read more

- SubmitAsync, polling results, timeouts/errors: [TUTORIAL.md](../../TUTORIAL.md)
- Canonical Task/Step/async reference: `../pick_and_place/uf_stage.cpp`
- Full example gallery: [EXAMPLES.md](../../EXAMPLES.md)
