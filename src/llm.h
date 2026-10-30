// llm.h -- client for a local language model server.
//
// Two backends speak different dialects:
//
//   LlamaCpp  the bundled llama-server, an OpenAI-compatible API
//             (/v1/chat/completions, /v1/embeddings, /health). This is the
//             default and needs nothing installed -- the binaries and weights
//             ship inside the application folder.
//
//   Ollama    /api/chat and /api/embed, for a machine that already runs Ollama
//             and would rather reuse its models than keep a second copy.
//
// Both are plain HTTP to loopback, so no TLS is involved.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Config;

namespace llm {

enum class Backend {
    LlamaCpp,
    Ollama,
};

Backend backend_from_string(const std::string& name);

struct Message {
    std::string role;  // "system" | "user" | "assistant"
    std::string content;
};

struct ChatOptions {
    std::string model;  // ignored by llama-server, which serves one model
    double temperature = 0.2;
    int num_ctx = 8192;
    int num_predict = -1;
    // JSON schema (or the literal "json") to constrain decoding.
    std::string format;
    // "" leaves the model default; otherwise "true"/"false"/"low"/"medium"/"high".
    //
    // With reasoning ON both backends keep it in a separate field and the
    // answer text stays clean, so the tutor gets the accuracy for free. With it
    // OFF the reasoning is emitted into the answer instead -- which is why it
    // is never disabled for the tutor, only for the classifier, whose reply is
    // one schema-constrained enum and whose token budget reasoning would eat.
    std::string think;
    int timeout_s = 300;
};

struct ChatResult {
    bool ok = false;
    std::string content;
    std::string error;
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
};

// Return false to abort generation.
using TokenCallback = std::function<bool(const std::string& chunk)>;

class Client {
public:
    explicit Client(const Config& config);

    // Overrides the configured endpoint. Used once the bundled runtime has
    // started and reported the port it actually bound.
    void set_chat_url(const std::string& url);
    void set_embed_url(const std::string& url);

    bool is_available(std::string* error = nullptr);
    std::vector<std::string> list_models(std::string* error = nullptr);
    bool has_model(const std::string& name, std::string* error = nullptr);

    ChatResult chat(const std::vector<Message>& messages, const ChatOptions& options);

    // `on_thinking` receives reasoning chunks. They are a liveness signal only:
    // never stored, never shown as the answer.
    ChatResult chat_stream(const std::vector<Message>& messages, const ChatOptions& options,
                           const TokenCallback& on_token,
                           const TokenCallback& on_thinking = nullptr);

    std::vector<std::vector<float>> embed(const std::vector<std::string>& inputs,
                                          std::string* error = nullptr);

    Backend backend() const { return backend_; }
    const std::string& chat_url() const { return chat_url_; }
    const std::string& default_model() const { return default_model_; }
    const std::string& embed_model() const { return embed_model_; }

private:
    struct Endpoint {
        std::string host = "127.0.0.1";
        int port = 8080;
    };
    Endpoint parse(const std::string& url) const;

    Backend backend_ = Backend::LlamaCpp;
    std::string chat_url_;
    std::string embed_url_;
    std::string default_model_;
    std::string embed_model_;
    int timeout_s_ = 300;
};

}  // namespace llm
