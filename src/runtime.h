// runtime.h -- supervises the bundled llama-server processes.
//
// The application ships its own inference engine and weights, so nothing has to
// be installed on the target machine. On startup this launches one llama-server
// for the tutor model and, when retrieval is in use, a second one for the
// embedding model, then hands the ports it actually bound back to the client.
//
// The children are placed in a Windows job object configured to kill on close.
// That is the part that matters operationally: a llama-server left running
// holds several gigabytes of RAM, and without the job object a crash of this
// process would orphan it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Config;

namespace runtime {

// Reports progress while models load, which takes tens of seconds.
using ProgressCallback = std::function<void(const std::string& message)>;

struct Status {
    bool chat_running = false;
    bool embed_running = false;
    std::string chat_url;
    std::string embed_url;
    std::string chat_model_path;
    std::string embed_model_path;
    std::string accelerator;  // "cpu" or "vulkan"
    std::string error;
};

// An answer-style choice offered in the UI. Only presets whose weights are
// actually present are produced.
struct Preset {
    std::string id;        // "fast" | "smart"
    std::string label;     // "Fast" | "Smart"
    std::string hint;      // one line for the UI
    std::string file;      // absolute path to the .gguf
    std::string filename;
    uint64_t size_bytes = 0;
};

class LocalRuntime {
public:
    explicit LocalRuntime(const Config& config);
    ~LocalRuntime();
    LocalRuntime(const LocalRuntime&) = delete;
    LocalRuntime& operator=(const LocalRuntime&) = delete;

    // True when the bundled engine and a chat model are both present. When this
    // is false the app falls back to whatever `llm.url` points at.
    bool available() const;

    // Launches the servers and blocks until they answer /health or time out.
    // `preset_id` selects which weights to load; empty uses the configured
    // default, falling back to whatever is present.
    bool start(const ProgressCallback& progress, std::string* error,
               const std::string& preset_id = "");
    void stop();

    // Presets whose weights exist, in the order the UI should list them.
    // May be empty (no bundled weights) or hold a single entry.
    const std::vector<Preset>& presets() const { return presets_; }
    // Id of the preset currently loaded, or "" before start().
    const std::string& active_preset() const { return active_preset_; }

    // Swaps the chat model. The old server is stopped and a new one started, so
    // this takes as long as a cold start and must not be called on the UI
    // thread. The embedding server is left alone. On failure the previous
    // preset is reloaded so the app is never left with no model.
    bool switch_preset(const std::string& preset_id, const ProgressCallback& progress,
                       std::string* error);

    const Status& status() const { return status_; }

    // Absolute path to the directory holding the runtime, for diagnostics.
    std::string runtime_dir() const { return runtime_dir_; }
    // Model files found in runtime/models, for the settings UI.
    std::string chat_model_name() const { return chat_model_name_; }

private:
    struct Impl;
    Impl* impl_;

    // Starts (or restarts) the chat server on `model_path`. Used by both
    // start() and switch_preset().
    bool launch_chat(const std::string& model_path, const ProgressCallback& progress,
                     std::string* error);

    std::string runtime_dir_;
    std::string chat_model_name_;
    std::string active_preset_;
    std::vector<Preset> presets_;
    Status status_;
};

}  // namespace runtime
