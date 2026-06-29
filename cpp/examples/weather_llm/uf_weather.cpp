// std::getenv is flagged C4996 by the MSVC CRT; it is the portable,
// standard way to read the optional GEMINI_API_KEY, so silence the warning.
#define _CRT_SECURE_NO_WARNINGS

#include "uf_weather.h"

#include "http_client.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace uniflow;

namespace
{
    // The KMA front page (weather.go.kr/w/index.do) renders its numbers
    // through JavaScript, so an HTTPS GET only returns the shell HTML
    // and the LLM cannot find anything to summarise. The legacy DFS
    // forecast endpoint below returns the same data as XML and works
    // straight from WinHTTP - grid (60, 127) is Seoul.
    constexpr const char* kKmaUrl =
        "https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127";

    // Free-tier model available on Google AI Studio.
    constexpr const char* kModel   = "gemini-2.5-flash";
    constexpr int         kMaxToks = 1024;
    constexpr std::size_t kBodyCap = 30000;
}

Flow_Weather::Flow_Weather(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Weather>(rt, "Flow_Weather")
{
    AddTask(task_fetch_);
}

// ======================================================================
//  Task: Fetch
// ======================================================================

StepResult Flow_Weather::Task_Fetch::Step1_Fetch()
{
    Describe("submitting GET to ", kKmaUrl);
    std::cout << "[weather] fetching " << kKmaUrl << "\n";
    // SubmitAsync: the HTTPS GET runs on the pool; the pump never blocks.
    // Carry the returned id to the continuation that polls the result.
    AsyncId id = SubmitAsync(UF_FN(FetchHtml), Duration::max(), std::string(kKmaUrl));
    if (id == 0)
    {
        std::cout << "[weather] fetch submission rejected\n";
        Describe("fetch rejected");
        return Fail();
    }
    return Next(UF_FN(Step2_AfterFetch), id);
}

StepResult Flow_Weather::Task_Fetch::Step2_AfterFetch(uniflow::AsyncId id)
{
    // Poll the GET by id. While the worker is still on the pool, Stay and
    // re-enter; the pump keeps running every other flow meanwhile.
    auto r = AsyncResult<std::string>(id);
    if (r.pending())
    {
        return Stay();
    }
    if (!r.ok())
    {
        std::cout << "[weather] HTTP fetch failed\n";
        Describe("fetch failed");
        return Fail();
    }
    flow().html_ = *r.return_value;
    std::cout << "[weather] got " << flow().html_.size() << " bytes of XML\n";
    Describe("got ", flow().html_.size(), " bytes");

    const char* key = std::getenv("GEMINI_API_KEY");
    if (!key || !*key)
    {
        std::cout << "\n[weather] GEMINI_API_KEY not set; printing first "
                  << "500 bytes of the fetched XML instead.\n"
                  << "          get a free key at "
                  << "https://aistudio.google.com/app/apikey\n\n";
        std::cout << flow().html_.substr(0, 500) << "\n";
        return Done();
    }

    std::string prompt = BuildPrompt(Truncate(flow().html_, kBodyCap));
    std::cout << "[weather] submitting POST to Gemini (" << kModel << ", "
              << "prompt=" << prompt.size() << " bytes)\n";
    Describe("submitting Gemini call");
    // SubmitAsync again: the Gemini POST is a second pool job, polled in Step3.
    AsyncId id2 = SubmitAsync(UF_FN(CallGemini), Duration::max(),
                              std::string(key), std::string(kModel),
                              std::move(prompt), kMaxToks);
    if (id2 == 0)
    {
        std::cout << "[weather] Gemini submission rejected\n";
        Describe("gemini rejected");
        return Fail();
    }
    return Next(UF_FN(Step3_AfterLlm), id2);
}

StepResult Flow_Weather::Task_Fetch::Step3_AfterLlm(uniflow::AsyncId id)
{
    // Poll the Gemini POST by id, same idiom as the fetch.
    auto r = AsyncResult<std::string>(id);
    if (r.pending())
    {
        return Stay();
    }
    if (!r.ok())
    {
        std::cout << "[weather] Gemini call failed\n";
        Describe("llm failed");
        return Fail();
    }

    const std::string& raw = *r.return_value;
    std::string text = HttpClient::ExtractGeminiText(raw);
    if (text == raw)
    {
        // Could not pick the text field out; the response was probably
        // an error envelope. Show the message field if present.
        std::string err = HttpClient::ExtractGeminiError(raw);
        if (!err.empty())
        {
            std::cout << "\n[weather] API returned error: " << err << "\n";
        }
        std::cout << "\n[weather] raw response (first 800 bytes):\n"
                  << raw.substr(0, 800) << "\n";
        return Fail();
    }

    std::cout << "\n=== Gemini summary ===\n"
              << text
              << "\n=== end ===\n";
    Describe("llm done, ", text.size(), " bytes");
    return Done();
}

std::string Flow_Weather::Task_Fetch::FetchHtml(std::string url)
{
    return HttpClient::Get(url);
}

std::string Flow_Weather::Task_Fetch::CallGemini(std::string api_key, std::string model,
                                                 std::string prompt, int max_output_tokens)
{
    return HttpClient::GeminiGenerate(api_key, model, prompt, max_output_tokens);
}

std::string Flow_Weather::Task_Fetch::BuildPrompt(const std::string& body)
{
    // The XML has a sequence of <data seq="N"> nodes, one per 3-hour
    // forecast window. seq=0 is the earliest still-relevant slot for
    // "now". Each node carries temp / sky / pty / wfKor / pop / ws /
    // wdKor / reh; the header carries the tm (issuance timestamp).
    //
    // Fine-dust (PM10 / PM2.5) is NOT in this feed - KMA serves it
    // through AirKorea, which needs an auth key. The prompt tells the
    // model to mark that field as not available so the answer stays
    // honest about what the source actually contained.
    std::string p;
    p += "The XML below is from the Korea Meteorological Administration's "
         "DFS forecast endpoint for Seoul (grid 60, 127): "
         "https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127.\n\n";
    p += "Treat <data seq=\"0\"> as the current forecast slot. From it, "
         "extract Seoul's weather snapshot. Reply in Korean, using this "
         "exact format:\n";
    p += "  location: <text>\n";
    p += "  temperature: <text, include the unit>\n";
    p += "  sky/precipitation: <text from wfKor, e.g. clear/cloudy/rain>\n";
    p += "  humidity: <text from reh, with %>\n";
    p += "  wind: <text combining wdKor and ws, with units>\n";
    p += "If a field is missing for any other reason, write '<not found>' "
         "for that field. Do not invent numbers.\n\n";
    p += "XML BEGIN\n";
    p += body;
    p += "\nXML END\n";
    return p;
}

std::string Flow_Weather::Task_Fetch::Truncate(const std::string& s, std::size_t n)
{
    if (s.size() <= n)
    {
        return s;
    }
    return s.substr(0, n);
}
