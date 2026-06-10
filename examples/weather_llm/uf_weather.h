// uf_weather.h - one Uniflow module, two async stages. The pump thread
// never blocks: the HTTPS GET and the Gemini generateContent POST both
// go to the executor pool via UF_ASYNC.
//
//   OnWeather_Begin -> OnWeather_AfterFetch -> OnWeather_AfterLlm -> Done
//
// Set GEMINI_API_KEY in the environment for the LLM call. If the key
// is missing the module prints a slice of the raw HTML and exits clean.
// Get a key for free from https://aistudio.google.com/app/apikey
#pragma once

#include "uniflow.hpp"

#include <string>

class UF_Weather : public uniflow::Uniflow<UF_Weather>
{
    UF_UNIFLOW_IMPLEMENT(UF_Weather);

public:
    explicit UF_Weather(uniflow::Runtime& rt)
        : uniflow::Uniflow<UF_Weather>(rt) {}

    StepResult OnWeather_Begin();

private:
    StepResult OnWeather_AfterFetch();
    StepResult OnWeather_AfterLlm();

    // Both targets run on the pool. They must be static - UF_ASYNC will
    // static_assert otherwise.
    static std::string FetchHtml(std::string url);
    static std::string CallGemini(std::string api_key, std::string model,
                                  std::string prompt, int max_output_tokens);

    static std::string BuildPrompt(const std::string& html_slice);
    static std::string Truncate(const std::string& s, std::size_t n);

    std::string html_;
};
