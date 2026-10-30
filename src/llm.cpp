#include "llm.h"

#include <cstdio>

#include "config.h"
#include "httplib.h"
#include "json.hpp"
#include "util.h"

using json = nlohmann::json;

namespace llm {
namespace {

// Reads a string field that may be absent, null, or a non-string.
//
// json::value() only substitutes the default when the key is *missing*. If the
// key is present and null it throws. llama-server relies on that case
// constantly: every streamed chunk carries both "content" and
// "reasoning_content", with whichever one is not being produced set to null.
std::string json_str(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// Same idea for integers.
int64_t json_int(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return 0;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return 0;
    return it->get<int64_t>();
}

// Returns the named child object, or an empty object when it is missing or null.
nlohmann::json json_obj(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return nlohmann::json::object();
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return nlohmann::json::object();
    return *it;
}

bool thinking_unsupported(const std::string& message) {
    const std::string lower = util::to_lower(message);
    return util::contains(lower, "does not support thinking") ||
           util::contains(lower, "thinking is not supported") ||
           (util::contains(lower, "think") && util::contains(lower, "not support"));
}

std::string http_failure(const httplib::Result& res, const std::string& what) {
    if (!res) {
        return what + ": cannot reach the model server (" + httplib::to_string(res.error()) + ")";
    }
    const std::string detail = util::utf8_truncate(res->body, 300);
    return what + ": server returned HTTP " + std::to_string(res->status) +
           (detail.empty() ? "" : " -- " + detail);
}

void apply_format(json* body, const std::string& format, Backend backend) {
    if (format.empty()) return;
    if (backend == Backend::Ollama) {
        if (format == "json") {
            (*body)["format"] = "json";
        } else {
            try {
                (*body)["format"] = json::parse(format);
            } catch (const json::exception&) {
                (*body)["format"] = "json";
            }
        }
        return;
    }
    // llama-server follows the OpenAI response_format shape.
    if (format == "json") {
        (*body)["response_format"] = {{"type", "json_object"}};
        return;
    }
    try {
        (*body)["response_format"] = {
            {"type", "json_schema"},
            {"json_schema", {{"name", "response"}, {"strict", true},
                             {"schema", json::parse(format)}}}};
    } catch (const json::exception&) {
        (*body)["response_format"] = {{"type", "json_object"}};
    }
}

void apply_think(json* body, const std::string& think, Backend backend) {
    if (think.empty()) return;
    if (backend == Backend::Ollama) {
        if (think == "true") (*body)["think"] = true;
        else if (think == "false") (*body)["think"] = false;
        else (*body)["think"] = think;
        return;
    }
    // llama-server drives Qwen-style reasoning through the chat template.
    const bool enabled = think != "false";
    (*body)["chat_template_kwargs"] = {{"enable_thinking", enabled}};
}

json build_body(const std::vector<Message>& messages, const ChatOptions& options, bool stream,
                Backend backend) {
    json body;
    json arr = json::array();
    for (const Message& m : messages) {
        arr.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["messages"] = arr;
    body["stream"] = stream;

    if (backend == Backend::Ollama) {
        body["model"] = options.model;
        // Keep the model resident; reloading multiple GB per question would
        // dominate response time.
        body["keep_alive"] = "30m";
        json opts;
        opts["temperature"] = options.temperature;
        opts["num_ctx"] = options.num_ctx;
        if (options.num_predict > 0) opts["num_predict"] = options.num_predict;
        body["options"] = opts;
    } else {
        // llama-server serves a single model, but the field is required by the
        // OpenAI schema.
        body["model"] = options.model.empty() ? "local" : options.model;
        body["temperature"] = options.temperature;
        if (options.num_predict > 0) body["max_tokens"] = options.num_predict;
    }

    apply_format(&body, options.format, backend);
    apply_think(&body, options.think, backend);
    return body;
}

}  // namespace

Backend backend_from_string(const std::string& name) {
    const std::string lower = util::to_lower(util::trim(name));
    if (lower == "ollama") return Backend::Ollama;
    return Backend::LlamaCpp;
}

Client::Client(const Config& config) {
    backend_ = backend_from_string(config.llm_backend());
    chat_url_ = config.llm_url();
    embed_url_ = config.llm_embed_url().empty() ? chat_url_ : config.llm_embed_url();
    default_model_ = config.ollama_model();
    embed_model_ = config.ollama_embed_model();
    timeout_s_ = config.ollama_timeout_s();
}

void Client::set_chat_url(const std::string& url) { chat_url_ = url; }
void Client::set_embed_url(const std::string& url) { embed_url_ = url; }

Client::Endpoint Client::parse(const std::string& url) const {
    Endpoint e;
    e.port = backend_ == Backend::Ollama ? 11434 : 8080;

    std::string rest = url;
    const size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    if (rest.empty()) return e;

    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        e.host = rest.substr(0, colon);
        try {
            e.port = std::stoi(rest.substr(colon + 1));
        } catch (const std::exception&) {
        }
    } else {
        e.host = rest;
    }
    if (e.host.empty()) e.host = "127.0.0.1";
    return e;
}

bool Client::is_available(std::string* error) {
    const Endpoint e = parse(chat_url_);
    httplib::Client cli(e.host, e.port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);

    const char* path = backend_ == Backend::Ollama ? "/api/tags" : "/health";
    auto res = cli.Get(path);
    if (!res || res->status != 200) {
        if (error) *error = http_failure(res, "health check");
        return false;
    }
    return true;
}

std::vector<std::string> Client::list_models(std::string* error) {
    std::vector<std::string> names;
    const Endpoint e = parse(chat_url_);
    httplib::Client cli(e.host, e.port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(10, 0);

    if (backend_ == Backend::Ollama) {
        auto res = cli.Get("/api/tags");
        if (!res || res->status != 200) {
            if (error) *error = http_failure(res, "list models");
            return names;
        }
        try {
            json parsed = json::parse(res->body);
            for (const auto& m : parsed.value("models", json::array())) {
                if (m.contains("name")) names.push_back(m["name"].get<std::string>());
            }
        } catch (const json::exception& ex) {
            if (error) *error = std::string("list models: bad JSON -- ") + ex.what();
        }
        return names;
    }

    auto res = cli.Get("/v1/models");
    if (!res || res->status != 200) {
        if (error) *error = http_failure(res, "list models");
        return names;
    }
    try {
        json parsed = json::parse(res->body);
        for (const auto& m : parsed.value("data", json::array())) {
            if (m.contains("id")) names.push_back(m["id"].get<std::string>());
        }
    } catch (const json::exception& ex) {
        if (error) *error = std::string("list models: bad JSON -- ") + ex.what();
    }
    return names;
}

bool Client::has_model(const std::string& name, std::string* error) {
    // llama-server serves whatever weights it was started with, so if it is
    // answering at all, the model is present.
    if (backend_ == Backend::LlamaCpp) return is_available(error);

    const std::vector<std::string> installed = list_models(error);
    const std::string want = util::to_lower(name);
    for (const std::string& have : installed) {
        const std::string lower = util::to_lower(have);
        if (lower == want) return true;
        if (want.find(':') == std::string::npos && util::starts_with(lower, want + ":")) return true;
    }
    return false;
}

ChatResult Client::chat(const std::vector<Message>& messages, const ChatOptions& options) {
    ChatResult out;
    ChatOptions opts = options;
    if (opts.model.empty()) opts.model = default_model_;

    const Endpoint e = parse(chat_url_);
    httplib::Client cli(e.host, e.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(opts.timeout_s > 0 ? opts.timeout_s : timeout_s_, 0);
    cli.set_write_timeout(30, 0);

    const char* path =
        backend_ == Backend::Ollama ? "/api/chat" : "/v1/chat/completions";

    std::string body = build_body(messages, opts, /*stream=*/false, backend_).dump();
    auto res = cli.Post(path, body, "application/json");

    if (res && res->status != 200 && !opts.think.empty() && thinking_unsupported(res->body)) {
        opts.think.clear();
        body = build_body(messages, opts, /*stream=*/false, backend_).dump();
        res = cli.Post(path, body, "application/json");
    }

    if (!res || res->status != 200) {
        out.error = http_failure(res, "chat");
        return out;
    }

    try {
        json parsed = json::parse(res->body);
        if (backend_ == Backend::Ollama) {
            out.content = json_str(json_obj(parsed, "message"), "content");
            out.prompt_tokens = json_int(parsed, "prompt_eval_count");
            out.completion_tokens = json_int(parsed, "eval_count");
        } else {
            if (parsed.contains("choices") && parsed["choices"].is_array() &&
                !parsed["choices"].empty()) {
                out.content = json_str(json_obj(parsed["choices"][0], "message"), "content");
            }
            const json usage = json_obj(parsed, "usage");
            out.prompt_tokens = json_int(usage, "prompt_tokens");
            out.completion_tokens = json_int(usage, "completion_tokens");
        }
        out.ok = true;
    } catch (const json::exception& ex) {
        out.error = std::string("chat: could not parse response -- ") + ex.what();
    }
    return out;
}

ChatResult Client::chat_stream(const std::vector<Message>& messages, const ChatOptions& options,
                               const TokenCallback& on_token, const TokenCallback& on_thinking) {
    ChatResult out;
    ChatOptions opts = options;
    if (opts.model.empty()) opts.model = default_model_;

    const Endpoint e = parse(chat_url_);
    httplib::Client cli(e.host, e.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(opts.timeout_s > 0 ? opts.timeout_s : timeout_s_, 0);
    cli.set_write_timeout(30, 0);

    const char* path = backend_ == Backend::Ollama ? "/api/chat" : "/v1/chat/completions";
    const std::string body = build_body(messages, opts, /*stream=*/true, backend_).dump();

    std::string pending;
    bool aborted = false;
    std::string parse_error;

    auto consume = [&](const std::string& reasoning, const std::string& piece, bool done_flag,
                       const json& obj) {
        if (!reasoning.empty() && on_thinking && !on_thinking(reasoning)) aborted = true;
        if (!piece.empty()) {
            out.content += piece;
            if (on_token && !on_token(piece)) aborted = true;
        }
        if (done_flag) {
            if (backend_ == Backend::Ollama) {
                out.prompt_tokens = obj.value("prompt_eval_count", 0);
                out.completion_tokens = obj.value("eval_count", 0);
            }
        }
    };

    auto handle_line = [&](std::string line) {
        line = util::trim(line);
        if (line.empty()) return;

        if (backend_ == Backend::LlamaCpp) {
            // Server-sent events: "data: {...}", terminated by "data: [DONE]".
            if (!util::starts_with(line, "data:")) return;
            line = util::trim(line.substr(5));
            if (line == "[DONE]") return;
        }

        try {
            json obj = json::parse(line);
            if (obj.contains("error") && !obj["error"].is_null()) {
                const auto& err = obj["error"];
                parse_error = err.is_string() ? err.get<std::string>() : json_str(err, "message");
                if (parse_error.empty()) parse_error = "model server error";
                return;
            }
            if (backend_ == Backend::Ollama) {
                const json message = json_obj(obj, "message");
                const bool done = obj.contains("done") && obj["done"].is_boolean() &&
                                  obj["done"].get<bool>();
                consume(json_str(message, "thinking"), json_str(message, "content"), done, obj);
            } else {
                if (!obj.contains("choices") || !obj["choices"].is_array()) return;
                const auto& choices = obj["choices"];
                if (choices.empty()) return;
                const json delta = json_obj(choices[0], "delta");
                consume(json_str(delta, "reasoning_content"), json_str(delta, "content"), false,
                        obj);
                const json usage = json_obj(obj, "usage");
                if (usage.contains("completion_tokens")) {
                    out.prompt_tokens = json_int(usage, "prompt_tokens");
                    out.completion_tokens = json_int(usage, "completion_tokens");
                }
            }
        } catch (const json::exception& ex) {
            parse_error = std::string("stream: bad JSON line -- ") + ex.what();
        }
    };

    auto receiver = [&](const char* data, size_t len) -> bool {
        pending.append(data, len);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            handle_line(pending.substr(0, nl));
            pending.erase(0, nl + 1);
            if (aborted) return false;
        }
        return !aborted;
    };

    auto res = cli.Post(path, httplib::Headers(), body, "application/json", receiver);
    if (!pending.empty() && !aborted) handle_line(pending);

    if (aborted) {
        // A caller-requested stop still leaves a usable partial answer.
        out.ok = true;
        return out;
    }
    if (!res || res->status != 200) {
        out.error = http_failure(res, "chat");
        return out;
    }
    if (!parse_error.empty()) {
        out.error = parse_error;
        return out;
    }
    out.ok = true;
    return out;
}

std::vector<std::vector<float>> Client::embed(const std::vector<std::string>& inputs,
                                              std::string* error) {
    std::vector<std::vector<float>> vectors;
    if (inputs.empty()) return vectors;

    const Endpoint e = parse(embed_url_);
    httplib::Client cli(e.host, e.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(timeout_s_, 0);
    cli.set_write_timeout(60, 0);

    json body;
    const char* path;
    if (backend_ == Backend::Ollama) {
        body["model"] = embed_model_;
        body["input"] = inputs;
        body["keep_alive"] = "30m";
        path = "/api/embed";
    } else {
        body["model"] = "local";
        body["input"] = inputs;
        path = "/v1/embeddings";
    }

    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res || res->status != 200) {
        if (error) *error = http_failure(res, "embed");
        return vectors;
    }

    try {
        json parsed = json::parse(res->body);
        if (backend_ == Backend::Ollama) {
            for (const auto& row : parsed.value("embeddings", json::array())) {
                std::vector<float> v;
                v.reserve(row.size());
                for (const auto& x : row) v.push_back(x.get<float>());
                vectors.push_back(std::move(v));
            }
        } else {
            // OpenAI shape: {"data":[{"index":0,"embedding":[...]}, ...]}.
            // The order of `data` is not guaranteed, so place by index.
            const auto data = parsed.value("data", json::array());
            vectors.resize(data.size());
            for (const auto& item : data) {
                const size_t index = item.value("index", 0);
                if (index >= vectors.size()) continue;
                std::vector<float> v;
                const auto& embedding = item["embedding"];
                v.reserve(embedding.size());
                for (const auto& x : embedding) v.push_back(x.get<float>());
                vectors[index] = std::move(v);
            }
        }
    } catch (const json::exception& ex) {
        if (error) *error = std::string("embed: could not parse response -- ") + ex.what();
        vectors.clear();
    }
    return vectors;
}

}  // namespace llm
