#include "runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include "config.h"
#include "httplib.h"
#include "util.h"

namespace fs = std::filesystem;

namespace runtime {
namespace {

// Ports are searched upward from here until a free pair is found, so a second
// copy of the app (or a leftover server) does not block startup.
constexpr int kChatPortBase = 11500;
constexpr int kEmbedPortBase = 11600;

// Directory containing the running executable. Everything the app ships with is
// resolved relative to this rather than the working directory, so shortcuts and
// "Run as administrator" (which changes the cwd) still work.
fs::path executable_dir() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0) return fs::current_path();
    return fs::path(std::wstring(buffer, n)).parent_path();
}

bool port_is_free(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);
    const bool free_port = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    closesocket(s);
    return free_port;
}

int find_free_port(int base) {
    for (int port = base; port < base + 60; ++port) {
        if (port_is_free(port)) return port;
    }
    return 0;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool health_ok(int port, const char* path) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(3, 0);
    auto res = cli.Get(path);
    return res && res->status == 200;
}

}  // namespace

struct LocalRuntime::Impl {
    HANDLE job = nullptr;
    HANDLE chat_process = nullptr;
    HANDLE embed_process = nullptr;

    std::string engine_dir;
    std::string chat_model;
    std::string embed_model;
    int chat_port = 0;
    int embed_port = 0;

    int context_size = 8192;
    int gpu_layers = 0;
    int threads = 0;
    int startup_timeout_s = 300;
    bool want_embeddings = true;

    // Launches llama-server. Returns the process handle, or nullptr.
    HANDLE spawn(const std::string& model_path, int port, bool embedding_mode,
                 std::string* error) const {
        const fs::path exe = fs::path(engine_dir) / "llama-server.exe";
        if (!fs::exists(exe)) {
            if (error) *error = "missing " + exe.string();
            return nullptr;
        }

        std::string command = "\"" + exe.string() + "\"";
        command += " -m \"" + model_path + "\"";
        command += " --host 127.0.0.1";
        command += " --port " + std::to_string(port);
        command += " -c " + std::to_string(embedding_mode ? 2048 : context_size);
        command += " -ngl " + std::to_string(gpu_layers);
        if (threads > 0) command += " -t " + std::to_string(threads);
        if (embedding_mode) {
            command += " --embedding --pooling mean";
        } else {
            // Keeps the model's reasoning in a separate `reasoning_content`
            // field instead of letting it run into the answer text.
            command += " --reasoning-format deepseek";
            command += " --jinja";
        }
        // Nothing reads the child's stdout; suppressing the banner keeps the
        // log file useful for real errors.
        command += " --log-disable";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        std::wstring wide_command = widen(command);
        std::wstring working_dir = widen(engine_dir);

        const BOOL ok = CreateProcessW(
            nullptr, wide_command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
            working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi);
        if (!ok) {
            if (error) {
                *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
            }
            return nullptr;
        }

        // Join the job before resuming, so the child can never outlive us even
        // if we are killed in the window between the two calls.
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    static bool still_running(HANDLE process) {
        if (!process) return false;
        return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
};

LocalRuntime::LocalRuntime(const Config& config) : impl_(new Impl) {
    const fs::path root = executable_dir();
    runtime_dir_ = (root / "runtime").string();

    // --- pick an accelerator ---------------------------------------------
    // Vulkan covers Intel, AMD and NVIDIA without a vendor-specific runtime, so
    // it is preferred when both are shipped and the user has not forced a
    // choice. It falls back to CPU if the machine has no Vulkan driver, which
    // is checked below by simply seeing whether the server starts.
    std::string preference = util::to_lower(config.get("runtime.accelerator", "auto"));
    const fs::path vulkan_dir = fs::path(runtime_dir_) / "bin" / "vulkan";
    const fs::path cpu_dir = fs::path(runtime_dir_) / "bin" / "cpu";

    if (preference == "vulkan" && fs::exists(vulkan_dir)) {
        impl_->engine_dir = vulkan_dir.string();
        status_.accelerator = "vulkan";
    } else if (preference == "cpu" || !fs::exists(vulkan_dir)) {
        impl_->engine_dir = cpu_dir.string();
        status_.accelerator = "cpu";
    } else {
        impl_->engine_dir = vulkan_dir.string();
        status_.accelerator = "vulkan";
    }

    // --- build the answer-style presets ------------------------------------
    const fs::path models_dir = fs::path(runtime_dir_) / "models";

    auto add_preset = [&](const std::string& id, const std::string& label,
                          const std::string& hint, const std::string& file_name) {
        if (file_name.empty()) return;
        const fs::path candidate = models_dir / file_name;
        std::error_code ec;
        if (!fs::exists(candidate, ec)) return;
        Preset p;
        p.id = id;
        p.label = label;
        p.hint = hint;
        p.file = candidate.string();
        p.filename = file_name;
        p.size_bytes = static_cast<uint64_t>(fs::file_size(candidate, ec));
        presets_.push_back(std::move(p));
    };

    // Fast first: it is the one a user reaches for when they are waiting.
    add_preset("fast", "Fast", "Quicker answers, weaker on multi-step problems",
               config.preset_fast_model());
    add_preset("smart", "Smart", "Slower, but more reliable working and arithmetic",
               config.preset_smart_model());

    // A model that is present but not named by any preset (the client swapped
    // the weights) still needs to be usable, so fall back to the largest
    // non-embedding file and offer it as a single unnamed choice.
    if (presets_.empty() && fs::exists(models_dir)) {
        uintmax_t best = 0;
        fs::path chosen;
        for (const auto& entry : fs::directory_iterator(models_dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".gguf") continue;
            const std::string name = entry.path().filename().string();
            if (util::contains(util::to_lower(name), "embed")) continue;
            const uintmax_t size = entry.file_size();
            if (size > best) {
                best = size;
                chosen = entry.path();
            }
        }
        if (!chosen.empty()) {
            Preset p;
            p.id = "default";
            p.label = "Default";
            p.hint = chosen.filename().string();
            p.file = chosen.string();
            p.filename = chosen.filename().string();
            p.size_bytes = static_cast<uint64_t>(best);
            presets_.push_back(std::move(p));
        }
    }

    // runtime.chat_model, when set, pins the model and overrides the presets.
    const std::string pinned = config.get("runtime.chat_model", "");
    if (!pinned.empty()) {
        const fs::path candidate = models_dir / pinned;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            presets_.clear();
            Preset p;
            p.id = "pinned";
            p.label = "Pinned";
            p.hint = pinned;
            p.file = candidate.string();
            p.filename = pinned;
            p.size_bytes = static_cast<uint64_t>(fs::file_size(candidate, ec));
            presets_.push_back(std::move(p));
        }
    }

    if (!presets_.empty()) {
        // Resolve the configured default, falling back to the last entry
        // (Smart, when both are present).
        const std::string wanted = config.preset_default();
        const Preset* chosen = &presets_.back();
        for (const Preset& p : presets_) {
            if (p.id == wanted) chosen = &p;
        }
        active_preset_ = chosen->id;
        impl_->chat_model = chosen->file;
        chat_model_name_ = chosen->filename;
    }

    std::string embed_name = config.get("runtime.embed_model", "nomic-embed-text.gguf");
    const fs::path embed_path = models_dir / embed_name;
    if (fs::exists(embed_path)) impl_->embed_model = embed_path.string();

    impl_->context_size = config.ollama_num_ctx();
    impl_->gpu_layers = static_cast<int>(config.get_int("runtime.gpu_layers", 99));
    impl_->threads = static_cast<int>(config.get_int("runtime.threads", 0));
    impl_->startup_timeout_s = static_cast<int>(config.get_int("runtime.startup_timeout_s", 300));
    impl_->want_embeddings = config.rag_enabled();

    status_.chat_model_path = impl_->chat_model;
    status_.embed_model_path = impl_->embed_model;
}

LocalRuntime::~LocalRuntime() {
    stop();
    delete impl_;
}

bool LocalRuntime::available() const {
    if (impl_->chat_model.empty()) return false;
    return fs::exists(fs::path(impl_->engine_dir) / "llama-server.exe");
}

bool LocalRuntime::launch_chat(const std::string& model_path, const ProgressCallback& progress,
                               std::string* error) {
    auto report = [&progress](const std::string& message) {
        if (progress) progress(message);
    };

    // Stop any previous chat server first: two of these hold several gigabytes
    // each, and the port would be taken.
    if (impl_->chat_process) {
        TerminateProcess(impl_->chat_process, 0);
        WaitForSingleObject(impl_->chat_process, 5000);
        CloseHandle(impl_->chat_process);
        impl_->chat_process = nullptr;
    }
    status_.chat_running = false;
    status_.chat_url.clear();

    impl_->chat_port = find_free_port(kChatPortBase);
    if (impl_->chat_port == 0) {
        status_.error = "no free local port for the model server";
        if (error) *error = status_.error;
        return false;
    }

    report("Starting " + fs::path(model_path).filename().string() + "...");

    std::string spawn_error;
    impl_->chat_process = impl_->spawn(model_path, impl_->chat_port, false, &spawn_error);

    // A machine with no Vulkan driver fails here; retry once on the CPU build.
    if (!impl_->chat_process && status_.accelerator == "vulkan") {
        const fs::path cpu_dir = fs::path(runtime_dir_) / "bin" / "cpu";
        if (fs::exists(cpu_dir / "llama-server.exe")) {
            report("GPU engine unavailable, falling back to CPU...");
            impl_->engine_dir = cpu_dir.string();
            status_.accelerator = "cpu";
            impl_->chat_process = impl_->spawn(model_path, impl_->chat_port, false, &spawn_error);
        }
    }

    if (!impl_->chat_process) {
        status_.error = "could not start the model server: " + spawn_error;
        if (error) *error = status_.error;
        return false;
    }

    // Loading several gigabytes off disk takes a while on first run.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(impl_->startup_timeout_s);
    bool ready = false;
    int elapsed = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!Impl::still_running(impl_->chat_process)) {
            // A Vulkan build on a machine without a driver exits rather than
            // failing to launch, so the fallback is also needed here.
            if (status_.accelerator == "vulkan") {
                const fs::path cpu_dir = fs::path(runtime_dir_) / "bin" / "cpu";
                if (fs::exists(cpu_dir / "llama-server.exe")) {
                    report("GPU engine exited, falling back to CPU...");
                    CloseHandle(impl_->chat_process);
                    impl_->engine_dir = cpu_dir.string();
                    status_.accelerator = "cpu";
                    impl_->chat_process =
                        impl_->spawn(model_path, impl_->chat_port, false, &spawn_error);
                    if (impl_->chat_process) continue;
                }
            }
            status_.error = "the model server exited during start-up";
            if (error) *error = status_.error;
            return false;
        }
        if (health_ok(impl_->chat_port, "/health")) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        elapsed += 1;
        if (elapsed % 8 == 0) {
            report("Loading the model into memory... (" + std::to_string(elapsed / 2) + "s)");
        }
    }

    if (!ready) {
        status_.error = "the model server did not become ready in time";
        if (error) *error = status_.error;
        return false;
    }

    impl_->chat_model = model_path;
    chat_model_name_ = fs::path(model_path).filename().string();
    status_.chat_model_path = model_path;
    status_.chat_running = true;
    status_.chat_url = "http://127.0.0.1:" + std::to_string(impl_->chat_port);
    return true;
}

bool LocalRuntime::switch_preset(const std::string& preset_id, const ProgressCallback& progress,
                                 std::string* error) {
    if (preset_id == active_preset_ && status_.chat_running) return true;

    const Preset* target = nullptr;
    for (const Preset& p : presets_) {
        if (p.id == preset_id) target = &p;
    }
    if (!target) {
        if (error) *error = "unknown answer style '" + preset_id + "'";
        return false;
    }

    const std::string previous_id = active_preset_;
    const std::string previous_model = impl_->chat_model;

    if (launch_chat(target->file, progress, error)) {
        active_preset_ = target->id;
        return true;
    }

    // Never leave the app with no model: put the previous one back.
    if (!previous_model.empty() && previous_model != target->file) {
        if (progress) progress("Could not load that model; restoring the previous one...");
        std::string ignored;
        if (launch_chat(previous_model, progress, &ignored)) {
            active_preset_ = previous_id;
        }
    }
    return false;
}

bool LocalRuntime::start(const ProgressCallback& progress, std::string* error,
                         const std::string& preset_id) {
    auto report = [&progress](const std::string& message) {
        if (progress) progress(message);
    };

    if (!available()) {
        status_.error =
            "No bundled model found. Expected a .gguf file in " +
            (fs::path(runtime_dir_) / "models").string();
        if (error) *error = status_.error;
        return false;
    }

    // Honour a remembered choice, if its weights are still present.
    if (!preset_id.empty()) {
        for (const Preset& p : presets_) {
            if (p.id == preset_id) {
                active_preset_ = p.id;
                impl_->chat_model = p.file;
                chat_model_name_ = p.filename;
            }
        }
    }

    // The job object owns every child; closing it kills them all.
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    if (impl_->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
    }

    if (!launch_chat(impl_->chat_model, progress, error)) return false;

    // --- embeddings (optional) --------------------------------------------
    if (impl_->want_embeddings && !impl_->embed_model.empty()) {
        impl_->embed_port = find_free_port(kEmbedPortBase);
        if (impl_->embed_port != 0) {
            report("Starting the textbook search index...");
            std::string embed_error;
            impl_->embed_process =
                impl_->spawn(impl_->embed_model, impl_->embed_port, true, &embed_error);
            if (impl_->embed_process) {
                const auto embed_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(120);
                while (std::chrono::steady_clock::now() < embed_deadline) {
                    if (!Impl::still_running(impl_->embed_process)) break;
                    if (health_ok(impl_->embed_port, "/health")) {
                        status_.embed_running = true;
                        status_.embed_url =
                            "http://127.0.0.1:" + std::to_string(impl_->embed_port);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                }
            }
        }
        // Retrieval degrades to keyword search without embeddings, so a failure
        // here is reported but never fatal.
        if (!status_.embed_running) {
            report("Textbook search will use keyword matching only.");
        }
    }

    return true;
}

void LocalRuntime::stop() {
    // Closing the job terminates every process assigned to it.
    if (impl_->job) {
        CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
    if (impl_->chat_process) {
        TerminateProcess(impl_->chat_process, 0);
        CloseHandle(impl_->chat_process);
        impl_->chat_process = nullptr;
    }
    if (impl_->embed_process) {
        TerminateProcess(impl_->embed_process, 0);
        CloseHandle(impl_->embed_process);
        impl_->embed_process = nullptr;
    }
    status_.chat_running = false;
    status_.embed_running = false;
}

}  // namespace runtime
