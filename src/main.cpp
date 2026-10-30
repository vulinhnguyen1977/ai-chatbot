// main.cpp -- entry point.
//
// Double-clicking the executable opens the desktop window. The subcommands
// below are for setup and diagnostics and are only useful from a terminal:
//
//   mstutor check      report on the bundled model, the corpus and email
//   mstutor index      build embeddings for the ingested textbooks
//   mstutor selftest   run the internal consistency checks
//
// The binary is linked as a GUI application so no console flashes up when it is
// launched from Explorer. For the subcommands we attach to the parent console
// (the terminal that ran us) and reconnect the C runtime's streams to it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "auth.h"
#include "chat.h"
#include "config.h"
#include "db.h"
#include "guard.h"
#include "gui.h"
#include "gui_richtext.h"  // widen()/narrow() for the command line and errors
#include "llm.h"
#include "mailer.h"
#include "rag.h"
#include "runtime.h"
#include "selftest.h"
#include "util.h"

namespace {

// Points a C runtime stream at whatever Win32 handle the process was given.
//
// A GUI-subsystem binary starts with no wired-up stdio, so printf goes nowhere.
// Reopening "CONOUT$" would fix the interactive case but would bypass any
// redirection the caller set up, so `mstutor check > out.txt` and
// `mstutor check | findstr ...` would both silently produce nothing. Binding to
// the inherited handle preserves both.
void bind_stream(DWORD which, FILE* stream, const char* mode) {
    HANDLE handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_TEXT);
    if (fd < 0) return;
    FILE* opened = _fdopen(fd, mode);
    if (!opened) return;
    setvbuf(opened, nullptr, _IONBF, 0);
    *stream = *opened;
}

// Reconnects stdio to the terminal that launched us, or to a new console when
// there is none.
void attach_console() {
    HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = existing && existing != INVALID_HANDLE_VALUE &&
                            GetFileType(existing) != FILE_TYPE_UNKNOWN;
    if (!redirected) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    }
    bind_stream(STD_OUTPUT_HANDLE, stdout, "w");
    bind_stream(STD_ERROR_HANDLE, stderr, "w");
    bind_stream(STD_INPUT_HANDLE, stdin, "r");
    // A fresh line so our output does not continue the shell's prompt line.
    std::printf("\n");
}

std::filesystem::path executable_dir() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(buffer, n)).parent_path();
}

void print_usage() {
    std::printf(
        "Middle School Maths & Physics Tutor\n\n"
        "Run with no arguments to open the app.\n\n"
        "Commands:\n"
        "  check       Report on the model, the textbook corpus and email\n"
        "  index       Build embeddings for the ingested textbooks\n"
        "  selftest    Run internal consistency checks\n\n"
        "Options:\n"
        "  --config <path>   Configuration file (default: config/config.ini)\n");
}

int run_check(Config& config, llm::Client& client, rag::Retriever& retriever, Mailer& mailer,
              runtime::LocalRuntime& local) {
    std::printf("Paths\n");
    std::printf("  database        %s\n", config.db_path().c_str());
    std::printf("  runtime         %s\n", local.runtime_dir().c_str());

    std::printf("\nBundled model runtime\n");
    if (local.available()) {
        std::printf("  engine          present (%s)\n", local.status().accelerator.c_str());
        std::printf("  chat weights    %s\n", local.chat_model_name().c_str());
        std::printf("  answer styles   ");
        if (local.presets().empty()) {
            std::printf("(none)\n");
        } else {
            for (size_t i = 0; i < local.presets().size(); ++i) {
                const runtime::Preset& p = local.presets()[i];
                std::printf("%s%s (%s, %.0f MB)", i ? ", " : "", p.label.c_str(),
                            p.filename.c_str(),
                            static_cast<double>(p.size_bytes) / (1024.0 * 1024.0));
            }
            std::printf("\n");
            std::printf("  starts on       %s\n", local.active_preset().c_str());
        }
        std::printf("  embed weights   %s\n",
                    local.status().embed_model_path.empty()
                        ? "(none -- retrieval will use keyword search)"
                        : std::filesystem::path(local.status().embed_model_path)
                              .filename()
                              .string()
                              .c_str());
        std::printf("  -> Nothing needs to be installed; the app starts this itself.\n");
    } else {
        std::printf("  NOT AVAILABLE\n");
        std::printf("  Expected llama-server.exe under runtime\\bin and a .gguf under "
                    "runtime\\models.\n");
        std::printf("  The app will fall back to %s\n", config.llm_url().c_str());
        std::string error;
        std::printf("  fallback reachable: %s\n",
                    client.is_available(&error) ? "yes" : ("no -- " + error).c_str());
    }

    rag::IndexStats stats = retriever.stats();
    std::printf("\nTextbook corpus\n");
    std::printf("  chunks          %lld\n", static_cast<long long>(stats.chunks));
    std::printf("  embedded        %lld\n", static_cast<long long>(stats.embedded));
    if (stats.chunks == 0) {
        std::printf("  -> Add PDFs to training\\data\\textbooks, then run:\n");
        std::printf("     python training\\1_extract_pdfs.py\n");
        std::printf("     python training\\2_build_corpus.py\n");
        std::printf("     python training\\3_ingest_corpus.py\n");
    } else if (stats.embedded < stats.chunks) {
        std::printf("  -> Run 'mstutor index' to embed the remaining %lld chunks\n",
                    static_cast<long long>(stats.chunks - stats.embedded));
    }

    std::printf("\nEmail\n");
    if (mailer.is_live()) {
        std::printf("  transport       SMTP (%s)\n", config.mail_host().c_str());
    } else {
        std::printf("  transport       FILE -- codes are written to %s\n",
                    mailer.outbox_dir().c_str());
        std::printf("  -> Set mail.transport=smtp and the mail.* settings to send real mail\n");
    }

    std::printf("\nSecurity\n");
    std::printf("  session length  %lld days\n", static_cast<long long>(config.session_days()));
    std::printf("  two-factor      %s\n", config.require_2fa() ? "on" : "off");
    return 0;
}

int run_index(rag::Retriever& retriever, llm::Client& client, runtime::LocalRuntime& local) {
    // Embedding needs the model server, which the desktop app would normally
    // have started; start it here for the duration of the command.
    if (local.available()) {
        std::printf("Starting the bundled model runtime...\n");
        std::string error;
        if (!local.start([](const std::string& m) { std::printf("  %s\n", m.c_str()); }, &error)) {
            std::fprintf(stderr, "Could not start the runtime: %s\n", error.c_str());
            return 1;
        }
        client.set_chat_url(local.status().chat_url);
        if (local.status().embed_running) client.set_embed_url(local.status().embed_url);
        if (!local.status().embed_running) {
            std::fprintf(stderr,
                         "No embedding model is available, so vectors cannot be built.\n"
                         "Retrieval will fall back to keyword search, which needs no index.\n");
            return 1;
        }
    }

    std::string error;
    if (!client.is_available(&error)) {
        std::fprintf(stderr, "The model server is not reachable: %s\n", error.c_str());
        return 1;
    }

    rag::IndexStats before = retriever.stats();
    if (before.chunks == 0) {
        std::printf("The corpus is empty. Ingest textbooks first (see training/README.md).\n");
        return 1;
    }
    std::printf("Embedding %lld chunks...\n",
                static_cast<long long>(before.chunks - before.embedded));

    int64_t last_percent = -1;
    const int64_t embedded = retriever.build_embeddings(
        [&last_percent](int64_t done, int64_t total) {
            const int64_t percent = total > 0 ? (done * 100 / total) : 100;
            if (percent != last_percent) {
                last_percent = percent;
                std::printf("\r  %lld%%  (%lld/%lld)   ", static_cast<long long>(percent),
                            static_cast<long long>(done), static_cast<long long>(total));
                std::fflush(stdout);
            }
        },
        &error);

    std::printf("\nEmbedded %lld chunks.\n", static_cast<long long>(embedded));
    if (!error.empty()) {
        std::fprintf(stderr, "Stopped early: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Per-monitor DPI keeps text crisp when the window is dragged between a
    // laptop screen and an external monitor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::string command;
    std::string config_path = "config/config.ini";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = gui::narrow(argv[i]);
        if (arg == "--config" && i + 1 < argc) {
            config_path = gui::narrow(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            command = "help";
        } else if (!arg.empty() && arg[0] != '-') {
            command = util::to_lower(arg);
        }
    }
    if (argv) LocalFree(argv);

    const bool console_mode = !command.empty();
    if (console_mode) attach_console();

    if (command == "help") {
        print_usage();
        return 0;
    }
    if (command == "selftest") {
        return selftest::run_all() ? 0 : 1;
    }

    // Resolve relative paths against the executable, not the working directory,
    // so shortcuts and "Run as administrator" behave the same as a double-click.
    std::error_code ec;
    std::filesystem::current_path(executable_dir(), ec);

    try {
        Config config;
        config.load(config_path);

        std::filesystem::path db_path(config.db_path());
        if (db_path.has_parent_path()) {
            std::filesystem::create_directories(db_path.parent_path(), ec);
        }

        db::Database database(config.db_path());
        database.migrate();

        Mailer mailer(config);
        Auth auth(database, config, mailer);
        llm::Client client(config);
        runtime::LocalRuntime local(config);
        guard::DomainGuard domain_guard(client, config);
        rag::Retriever retriever(database, client, config);
        chat::ChatService chats(database, client, domain_guard, retriever, config);

        if (command == "check") {
            return run_check(config, client, retriever, mailer, local);
        }
        if (command == "index") {
            return run_index(retriever, client, local);
        }
        if (!command.empty()) {
            std::fprintf(stderr, "Unknown command: %s\n\n", command.c_str());
            print_usage();
            return 2;
        }

        auth.purge_expired();

        gui::AppContext context{config, database, auth, chats, client, retriever, mailer, local};
        const int result = gui::run(context);

        // Terminates the bundled model server; leaving it running would hold
        // several gigabytes of memory after the window closed.
        local.stop();
        return result;
    } catch (const std::exception& e) {
        const std::wstring message =
            L"The application could not start.\n\n" + gui::widen(e.what());
        if (console_mode) {
            std::fprintf(stderr, "[fatal] %s\n", e.what());
        } else {
            MessageBoxW(nullptr, message.c_str(), L"Maths & Physics Tutor", MB_ICONERROR);
        }
        return 1;
    }
}
