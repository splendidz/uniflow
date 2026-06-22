// uf_weather.h - one Uniflow module, two chained async stages. The pump
// thread never blocks: the HTTPS GET and the Gemini generateContent POST
// both run on the Runtime's thread pool via SubmitAsync, and each result
// is polled by id in a continuation step.
//
//   Step1_Fetch -> Step2_AfterFetch -> Step3_AfterLlm -> Done
//
// Set GEMINI_API_KEY in the environment for the LLM call. If the key is
// missing the module prints a slice of the raw XML and exits clean.
// Get a key for free from https://aistudio.google.com/app/apikey
#pragma once

#include "uniflow.hpp"

#include <string>

class Flow_Weather : public uniflow::Uniflow<Flow_Weather>
{
public:
    explicit Flow_Weather(uniflow::Runtime& rt);

    // Fetch task: GET the KMA XML, then (optionally) summarise via Gemini.
    // Public so app.Start() can launch it with ctx_fetch_.StartFlow().
    struct Task_Fetch : uniflow::Task<Flow_Weather>
    {
        StepResult Entry() override { return Step1_Fetch(); }

    private:
        StepResult Step1_Fetch();
        StepResult Step2_AfterFetch(uniflow::AsyncId id);
        StepResult Step3_AfterLlm(uniflow::AsyncId id);

        // Both targets run on the pool. They are static so SubmitAsync can
        // take a plain function pointer that does not capture 'this'.
        static std::string FetchHtml(std::string url);
        static std::string CallGemini(std::string api_key, std::string model,
                                      std::string prompt, int max_output_tokens);

        static std::string BuildPrompt(const std::string& body);
        static std::string Truncate(const std::string& s, std::size_t n);
    } ctx_fetch_;

private:
    std::string html_;
};
