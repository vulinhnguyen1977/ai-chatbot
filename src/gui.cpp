#include "gui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>

#include "credential_store.h"
#include "gui_edit.h"
#include "gui_richtext.h"
#include "json.hpp"
#include "prefs.h"
#include "prompts.h"
#include "util.h"

using json = nlohmann::json;

namespace gui {
namespace {

// ---------------------------------------------------------------- constants

const wchar_t* kWindowClass = L"MsTutorMainWindow";
const wchar_t* kPromptClass = L"MsTutorPromptWindow";
const wchar_t* kAppTitle = L"Maths & Physics Tutor";

enum ControlId : int {
    // authentication
    kIdAuthEmail = 1001,
    kIdAuthPassword,
    kIdAuthName,
    kIdAuthCode,
    kIdAuthPrimary,
    kIdAuthSwitch,
    kIdAuthResend,
    kIdAuthRemember,

    // chat
    kIdChatList = 1101,
    kIdNewChat,
    kIdRenameChat,
    kIdDeleteChat,
    kIdAccount,
    kIdInput,
    kIdSend,
    kIdStop,
    kIdPreset,
    kIdPresetLabel,
    kIdStatus,

    // Accelerator-only commands (no control of their own).
    kIdAccelNextPane = 1150,
    kIdAccelPrevPane,
    kIdAccelFocusInput,
    kIdAccelShortcuts,
    kIdAccelPresetFast,
    kIdAccelPresetSmart,
    kIdAccelNextChat,
    kIdAccelPrevChat,
    kIdAccelClose,

    // prompt dialog
    kIdPromptEdit = 1201,
    kIdPromptOk,
    kIdPromptCancel,
};

enum : UINT {
    WM_APP_STREAM = WM_APP + 1,    // generation produced output
    WM_APP_DONE = WM_APP + 2,      // generation finished
    WM_APP_RUNTIME = WM_APP + 3,   // model server progress
    WM_APP_READY = WM_APP + 4,     // model server up (or failed)
    WM_APP_SWITCHED = WM_APP + 5,  // answer-style change finished
};

enum class AuthMode { Login, SignUp, Verify };

// ------------------------------------------------------------------ helpers

int scaled(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

void set_text(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

std::wstring get_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return buffer;
}

void message_box(HWND parent, const std::wstring& text, const std::wstring& caption, UINT flags) {
    MessageBoxW(parent, text.c_str(), caption.c_str(), flags);
}

// ----------------------------------------------------- small input dialog
// Used for renaming a chat and for entering emailed codes. A purpose-built
// window keeps the app free of resource scripts, which would otherwise be the
// only thing in the tree needing a resource compiler.

struct PromptState {
    std::wstring label;
    std::wstring value;
    bool accepted = false;
    bool password = false;
    HFONT font = nullptr;
};

LRESULT CALLBACK prompt_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            state = static_cast<PromptState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            const UINT dpi = GetDpiForWindow(hwnd);
            CreateWindowExW(0, L"STATIC", state->label.c_str(), WS_CHILD | WS_VISIBLE,
                            scaled(14, dpi), scaled(12, dpi), scaled(330, dpi), scaled(38, dpi),
                            hwnd, nullptr, nullptr, nullptr);
            DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
            if (state->password) style |= ES_PASSWORD;
            HWND edit = CreateWindowExW(0, L"EDIT", state->value.c_str(), style, scaled(14, dpi),
                                        scaled(54, dpi), scaled(330, dpi), scaled(26, dpi), hwnd,
                                        reinterpret_cast<HMENU>(kIdPromptEdit), nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"OK",
                            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, scaled(178, dpi),
                            scaled(92, dpi), scaled(80, dpi), scaled(28, dpi), hwnd,
                            reinterpret_cast<HMENU>(kIdPromptOk), nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, scaled(264, dpi),
                            scaled(92, dpi), scaled(80, dpi), scaled(28, dpi), hwnd,
                            reinterpret_cast<HMENU>(kIdPromptCancel), nullptr, nullptr);

            EnumChildWindows(hwnd,
                             [](HWND child, LPARAM font) -> BOOL {
                                 SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
                                 return TRUE;
                             },
                             reinterpret_cast<LPARAM>(state->font));
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wparam) == kIdPromptOk) {
                if (state) {
                    state->value = get_text(GetDlgItem(hwnd, kIdPromptEdit));
                    state->accepted = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == kIdPromptCancel) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Shows a modal text prompt. Returns false when cancelled.
bool prompt_text(HWND parent, HFONT font, const std::wstring& title, const std::wstring& label,
                 std::wstring* value, bool password = false) {
    PromptState state;
    state.label = label;
    state.value = *value;
    state.password = password;
    state.font = font;

    const UINT dpi = GetDpiForWindow(parent);
    const int width = scaled(372, dpi);
    const int height = scaled(170, dpi);

    RECT parent_rect{};
    GetWindowRect(parent, &parent_rect);
    const int x = parent_rect.left + ((parent_rect.right - parent_rect.left) - width) / 2;
    const int y = parent_rect.top + ((parent_rect.bottom - parent_rect.top) - height) / 3;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kPromptClass, title.c_str(),
        WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE, x, y, width, height, parent,
        nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd) return false;

    // Disabling the owner is what makes this modal; it is re-enabled before the
    // window is destroyed so focus returns correctly.
    EnableWindow(parent, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, kIdPromptOk, 0);
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (state.accepted) *value = state.value;
    return state.accepted;
}

// --------------------------------------------------------------- the window

class MainWindow {
public:
    explicit MainWindow(AppContext& context) : ctx_(context) {}

    bool create();
    int run_message_loop();

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK input_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK chat_list_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR id, DWORD_PTR data);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void build_controls();
    void layout();
    void show_auth(AuthMode mode);
    void show_chat();
    void refresh_auth_visibility();

    void do_auth_primary();
    void do_auth_switch();
    void do_resend();
    void apply_auth_result(const AuthResult& result, const std::wstring& fallback);

    // "Keep me signed in": persist / restore / discard the session token.
    void remember_session(const AuthResult& result);
    bool try_restore_session();
    void forget_session();

    void load_chats();
    void open_selected_chat();
    void render_chat();
    void new_chat();
    void rename_chat();
    void delete_chat();
    void show_account();

    void send_question();
    void stop_generation();
    void on_stream_update();
    void on_generation_done();

    // Keyboard: accelerator table, pane cycling, and the shortcut reference.
    void build_accelerators();
    void cycle_pane(bool forward);
    void focus_input();
    void show_shortcuts();
    void select_chat_relative(int delta);

    void populate_presets();
    void on_preset_changed();
    void on_preset_switched();
    void set_preset_enabled(bool enabled);

    void start_runtime();
    void set_status(const std::wstring& text);

    AppContext& ctx_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    Palette palette_{};
    HFONT font_ = nullptr;
    HFONT font_bold_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    HBRUSH surface_brush_ = nullptr;
    HMODULE richedit_library_ = nullptr;

    // authentication page
    HWND auth_panel_ = nullptr;
    HWND auth_title_ = nullptr;
    HWND auth_subtitle_ = nullptr;
    HWND auth_name_label_ = nullptr, auth_name_ = nullptr;
    HWND auth_email_label_ = nullptr, auth_email_ = nullptr;
    HWND auth_password_label_ = nullptr, auth_password_ = nullptr;
    HWND auth_code_label_ = nullptr, auth_code_ = nullptr;
    HWND auth_primary_ = nullptr, auth_switch_ = nullptr, auth_resend_ = nullptr;
    HWND auth_remember_ = nullptr;
    HWND auth_message_ = nullptr;
    AuthMode auth_mode_ = AuthMode::Login;
    std::string challenge_id_;
    // Where the encrypted "keep me signed in" token lives.
    std::string remember_path_;
    // The checkbox is on the first screen, but the session only exists after
    // the 2FA step, so the choice has to survive the screen change.
    bool remember_me_ = false;

    // chat page
    HWND chat_list_ = nullptr;
    HWND new_chat_ = nullptr, rename_chat_ = nullptr, delete_chat_ = nullptr, account_ = nullptr;
    HWND transcript_ = nullptr;
    HWND input_ = nullptr, send_ = nullptr, stop_ = nullptr;
    HWND status_ = nullptr;
    HWND preset_label_ = nullptr, preset_ = nullptr;

    bool signed_in_ = false;
    User user_{};
    std::vector<chat::ChatSummary> chats_;
    int64_t active_chat_ = 0;

    // generation state, shared with the worker thread
    std::thread worker_;
    std::mutex stream_mutex_;
    std::string stream_text_;
    size_t reasoning_chars_ = 0;
    std::atomic<bool> generating_{false};
    std::atomic<bool> cancel_{false};
    long stream_anchor_ = 0;
    bool stream_started_ = false;
    std::string pending_question_;
    chat::AskResult last_result_;

    std::thread runtime_thread_;
    std::mutex runtime_mutex_;
    std::wstring runtime_message_;
    bool runtime_ready_ = false;

    HACCEL accelerators_ = nullptr;

    // answer-style switching
    std::thread switch_thread_;
    std::atomic<bool> switching_{false};
    std::string requested_preset_;
    std::string switch_error_;
    // Index the dropdown should fall back to if a switch fails.
    int last_preset_index_ = 0;
};

MainWindow* g_window = nullptr;

LRESULT CALLBACK MainWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_window) return g_window->handle(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Enter sends, Shift+Enter inserts a newline. Multi-line questions matter here:
// students paste working out across several lines.
LRESULT CALLBACK MainWindow::input_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR id, DWORD_PTR data) {
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!shift) {
            auto* self = reinterpret_cast<MainWindow*>(data);
            if (self) self->send_question();
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, input_proc, id);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MainWindow::chat_list_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);

    if (message == WM_KEYDOWN && self) {
        switch (wparam) {
            case VK_RETURN:
                // The list already tracks the highlighted row with the arrow
                // keys; Enter is what opens it.
                self->open_selected_chat();
                return 0;
            case VK_DELETE:
                // Plain Delete is handled here rather than as an accelerator so
                // it only removes a chat when the list has focus, and still
                // deletes characters while typing.
                self->delete_chat();
                return 0;
            case VK_F2:
                self->rename_chat();
                return 0;
            default:
                break;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, chat_list_proc, id);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

bool MainWindow::create() {
    g_window = this;
    palette_ = palette_for_system();

    // RichEdit 4.1 lives in this DLL and must be loaded before the class name
    // can be used.
    richedit_library_ = LoadLibraryW(L"Msftedit.dll");
    if (!richedit_library_) {
        MessageBoxW(nullptr, L"Could not load the text renderer (Msftedit.dll).", kAppTitle,
                    MB_ICONERROR);
        return false;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted in WM_ERASEBKGND
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    WNDCLASSEXW pc{};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = prompt_proc;
    pc.hInstance = instance;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    pc.lpszClassName = kPromptClass;
    RegisterClassExW(&pc);

    hwnd_ = CreateWindowExW(0, kWindowClass, kAppTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, 1040, 720, nullptr, nullptr, instance, nullptr);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

int MainWindow::run_message_loop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Order matters. Accelerators are checked first so an application
        // shortcut wins over a control's own handling of the same key.
        // IsDialogMessage comes next: it is what implements Tab/Shift+Tab
        // between WS_TABSTOP controls, arrow keys within a group, Alt+access
        // keys, and Enter/Escape for the default and cancel buttons.
        if (accelerators_ && TranslateAcceleratorW(hwnd_, accelerators_, &msg)) continue;
        if (IsDialogMessageW(hwnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void MainWindow::build_controls() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    // Controls are created hidden; show_auth()/show_chat() reveal the page in
    // use, so both sets can live on the same window without a tab control.
    // SS_NOPREFIX: without it a STATIC treats '&' as a keyboard mnemonic, so
    // "Maths & Physics Tutor" renders as "Maths _Physics Tutor".
    auto make_static = [&](const wchar_t* text, DWORD extra = 0) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | SS_NOPREFIX | extra, 0, 0, 0, 0,
                               hwnd_, nullptr, instance, nullptr);
    };
    // A label whose '&' IS a mnemonic. Windows moves focus to the next control
    // in tab order when the access key is pressed, so each of these is created
    // immediately before the field it labels.
    auto make_label = [&](const wchar_t* text) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd_, nullptr,
                               instance, nullptr);
    };
    // WS_TABSTOP is what puts a control in the Tab cycle that IsDialogMessage
    // walks. Without it the keyboard cannot reach the field at all.
    auto make_edit = [&](int id, DWORD extra) {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | extra, 0, 0, 0, 0,
                                    hwnd_, reinterpret_cast<HMENU>(id), instance, nullptr);
        gui::install_edit_key_fixes(edit);
        return edit;
    };
    auto make_button = [&](int id, const wchar_t* text, DWORD extra = 0) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | extra, 0, 0, 0, 0,
                               hwnd_, reinterpret_cast<HMENU>(id), instance, nullptr);
    };

    // --- authentication ---------------------------------------------------
    auth_title_ = make_static(L"Maths & Physics Tutor");
    auth_subtitle_ = make_static(L"Middle school. Step by step.");
    // Each label carries an access key and is created immediately before its
    // field, which is what makes Alt+<letter> land in the right box.
    auth_name_label_ = make_label(L"&Name");
    auth_name_ = make_edit(kIdAuthName, 0);
    auth_email_label_ = make_label(L"&Email");
    auth_email_ = make_edit(kIdAuthEmail, 0);
    auth_password_label_ = make_label(L"&Password");
    auth_password_ = make_edit(kIdAuthPassword, ES_PASSWORD);
    auth_code_label_ = make_label(L"Verification &code");
    auth_code_ = make_edit(kIdAuthCode, ES_NUMBER | ES_CENTER);
    SendMessageW(auth_code_, EM_SETLIMITTEXT, 6, 0);
    auth_remember_ = make_button(kIdAuthRemember, L"&Keep me signed in on this device",
                                 BS_AUTOCHECKBOX);
    auth_primary_ = make_button(kIdAuthPrimary, L"&Sign in", BS_DEFPUSHBUTTON);
    auth_switch_ = make_button(kIdAuthSwitch, L"Create an acc&ount");
    auth_resend_ = make_button(kIdAuthResend, L"Send a new co&de");
    auth_message_ = make_static(L"");

    // --- chat -------------------------------------------------------------
    chat_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                                 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdChatList), instance,
                                 nullptr);
    // Enter opens, F2 renames and Delete removes the highlighted chat.
    SetWindowSubclass(chat_list_, chat_list_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    new_chat_ = make_button(kIdNewChat, L"&New chat");
    rename_chat_ = make_button(kIdRenameChat, L"&Rename");
    delete_chat_ = make_button(kIdDeleteChat, L"&Delete");
    account_ = make_button(kIdAccount, L"A&ccount");

    // WS_TABSTOP so the transcript can be reached and scrolled from the
    // keyboard. It is read-only, so focus there is safe.
    transcript_ = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                                  WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                      ES_AUTOVSCROLL | ES_NOHIDESEL,
                                  0, 0, 0, 0, hwnd_, nullptr, instance, nullptr);
    SendMessageW(transcript_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(palette_.background));
    SendMessageW(transcript_, EM_SETEVENTMASK, 0, ENM_NONE);
    // Without this the control silently stops accepting text at 32 KB, which a
    // long tutoring session reaches easily.
    SendMessageW(transcript_, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);
    // The transcript is a multi-line control too, so Tab would otherwise be
    // swallowed there as well and trap focus in the answer pane.
    gui::install_edit_key_fixes(transcript_);

    input_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                 ES_WANTRETURN,
                             0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdInput), instance,
                             nullptr);
    SetWindowSubclass(input_, input_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    // Chains a second subclass for the Ctrl+Backspace / Ctrl+Delete fix; the
    // two use different ids so both stay installed.
    gui::install_edit_key_fixes(input_);

    send_ = make_button(kIdSend, L"&Send", BS_DEFPUSHBUTTON);
    stop_ = make_button(kIdStop, L"Sto&p");
    // Given an id so it can be addressed directly rather than by position.
    status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0,
                              0, hwnd_, reinterpret_cast<HMENU>(kIdStatus), instance, nullptr);

    // Answer style. CBS_DROPDOWNLIST makes it a fixed choice rather than an
    // editable combo, so the user cannot type a model name that does not exist.
    preset_label_ = make_label(L"Answer st&yle");
    preset_ = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP, 0, 0, 0, 0,
                              hwnd_, reinterpret_cast<HMENU>(kIdPreset), instance, nullptr);

    // --- fonts -------------------------------------------------------------
    const int height = -MulDiv(10, static_cast<int>(dpi_), 72);
    font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    font_bold_ = CreateFontW(-MulDiv(15, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");

    EnumChildWindows(hwnd_,
                     [](HWND control, LPARAM font) -> BOOL {
                         SendMessageW(control, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
                         return TRUE;
                     },
                     reinterpret_cast<LPARAM>(font_));
    SendMessageW(auth_title_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

    background_brush_ = CreateSolidBrush(palette_.background);
    surface_brush_ = CreateSolidBrush(palette_.surface);
}

void MainWindow::refresh_auth_visibility() {
    const bool login = auth_mode_ == AuthMode::Login;
    const bool signup = auth_mode_ == AuthMode::SignUp;
    const bool verify = auth_mode_ == AuthMode::Verify;

    auto show = [](HWND control, bool visible) {
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    };

    show(auth_name_label_, signup);
    show(auth_name_, signup);
    show(auth_email_label_, !verify);
    show(auth_email_, !verify);
    show(auth_password_label_, !verify);
    show(auth_password_, !verify);
    show(auth_code_label_, verify);
    show(auth_code_, verify);
    show(auth_resend_, verify);
    // Offered on every screen that ends in a session, so the choice can be made
    // (or changed) right up to the moment it takes effect.
    show(auth_remember_, ctx_.config.remember_enabled());
    SendMessageW(auth_remember_, BM_SETCHECK, remember_me_ ? BST_CHECKED : BST_UNCHECKED, 0);

    // These two buttons are relabelled for each screen, so the access keys have
    // to be part of every variant -- setting the text without an '&' silently
    // removes the mnemonic that build_controls() installed.
    // Letters are chosen not to collide with the fields on the same screen
    // (N)ame, (E)mail, (P)assword, verification (c)ode, (K)eep me signed in.
    set_text(auth_primary_, login ? L"&Sign in" : (signup ? L"Create &account" : L"&Verify"));
    set_text(auth_switch_, login ? L"Create an acc&ount"
                                 : (signup ? L"I already have an acc&ount" : L"Start &over"));
    layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::show_auth(AuthMode mode) {
    signed_in_ = false;
    auth_mode_ = mode;

    for (HWND control : {auth_title_, auth_subtitle_, auth_primary_, auth_switch_, auth_message_}) {
        ShowWindow(control, SW_SHOW);
    }
    for (HWND control : {chat_list_, new_chat_, rename_chat_, delete_chat_, account_, transcript_,
                         input_, send_, stop_, status_, preset_label_, preset_}) {
        ShowWindow(control, SW_HIDE);
    }
    refresh_auth_visibility();
    SetFocus(mode == AuthMode::Verify ? auth_code_ : auth_email_);
}

void MainWindow::show_chat() {
    signed_in_ = true;
    for (HWND control : {auth_title_, auth_subtitle_, auth_name_label_, auth_name_,
                         auth_email_label_, auth_email_, auth_password_label_, auth_password_,
                         auth_code_label_, auth_code_, auth_primary_, auth_switch_, auth_resend_,
                         auth_remember_, auth_message_}) {
        ShowWindow(control, SW_HIDE);
    }
    for (HWND control : {chat_list_, new_chat_, rename_chat_, delete_chat_, account_, transcript_,
                         input_, send_, status_}) {
        ShowWindow(control, SW_SHOW);
    }
    ShowWindow(stop_, SW_HIDE);

    // Only worth showing when there is a real choice to make.
    const bool offer_presets = ctx_.runtime.presets().size() > 1;
    ShowWindow(preset_label_, offer_presets ? SW_SHOW : SW_HIDE);
    ShowWindow(preset_, offer_presets ? SW_SHOW : SW_HIDE);
    if (offer_presets) populate_presets();

    load_chats();
    active_chat_ = 0;
    render_chat();
    layout();
    SetFocus(input_);
}

void MainWindow::layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int pad = scaled(12, dpi_);

    if (!signed_in_) {
        const int panel_width = scaled(360, dpi_);
        const int x = (width - panel_width) / 2;
        int y = (height - scaled(430, dpi_)) / 2;
        if (y < pad) y = pad;

        const int row = scaled(26, dpi_);
        const int gap = scaled(6, dpi_);
        const int label_h = scaled(18, dpi_);
        if (y < pad) y = pad;

        MoveWindow(auth_title_, x, y, panel_width, scaled(30, dpi_), TRUE);
        y += scaled(34, dpi_);
        MoveWindow(auth_subtitle_, x, y, panel_width, scaled(20, dpi_), TRUE);
        y += scaled(30, dpi_);

        auto place = [&](HWND label, HWND edit, bool visible) {
            if (!visible) return;
            MoveWindow(label, x, y, panel_width, label_h, TRUE);
            y += label_h;
            MoveWindow(edit, x, y, panel_width, row, TRUE);
            y += row + gap;
        };

        place(auth_name_label_, auth_name_, auth_mode_ == AuthMode::SignUp);
        place(auth_email_label_, auth_email_, auth_mode_ != AuthMode::Verify);
        place(auth_password_label_, auth_password_, auth_mode_ != AuthMode::Verify);
        place(auth_code_label_, auth_code_, auth_mode_ == AuthMode::Verify);

        if (ctx_.config.remember_enabled()) {
            y += gap;
            MoveWindow(auth_remember_, x, y, panel_width, scaled(22, dpi_), TRUE);
            y += scaled(24, dpi_);
        }

        y += gap;
        MoveWindow(auth_primary_, x, y, panel_width, scaled(32, dpi_), TRUE);
        y += scaled(38, dpi_);
        if (auth_mode_ == AuthMode::Verify) {
            MoveWindow(auth_resend_, x, y, panel_width, scaled(26, dpi_), TRUE);
            y += scaled(30, dpi_);
        }
        MoveWindow(auth_switch_, x, y, panel_width, scaled(26, dpi_), TRUE);
        y += scaled(34, dpi_);
        MoveWindow(auth_message_, x, y, panel_width, scaled(90, dpi_), TRUE);
        return;
    }

    const int sidebar = scaled(220, dpi_);
    const int button_h = scaled(28, dpi_);
    const int status_h = scaled(22, dpi_);
    const int input_h = scaled(84, dpi_);

    // sidebar
    const bool offer_presets = IsWindowVisible(preset_) != 0;
    const int label_h = scaled(16, dpi_);
    const int combo_h = scaled(26, dpi_);
    const int gap = scaled(6, dpi_);
    // Everything below the chat list is fixed height; the list takes the rest.
    const int below = button_h * 2 + gap * 3 +
                      (offer_presets ? label_h + combo_h + gap : 0);

    int y = pad;
    MoveWindow(new_chat_, pad, y, sidebar, button_h, TRUE);
    y += button_h + gap;

    int list_h = height - y - pad - below;
    if (list_h < scaled(60, dpi_)) list_h = scaled(60, dpi_);
    MoveWindow(chat_list_, pad, y, sidebar, list_h, TRUE);
    y += list_h + gap;

    const int half = (sidebar - gap) / 2;
    MoveWindow(rename_chat_, pad, y, half, button_h, TRUE);
    MoveWindow(delete_chat_, pad + half + gap, y, half, button_h, TRUE);
    y += button_h + gap;

    if (offer_presets) {
        MoveWindow(preset_label_, pad, y, sidebar, label_h, TRUE);
        y += label_h;
        // A combo box's height argument sizes the dropped-down list, not the
        // closed control, so it must include room for the items.
        MoveWindow(preset_, pad, y, sidebar, combo_h + scaled(90, dpi_), TRUE);
        y += combo_h + gap;
    }

    MoveWindow(account_, pad, y, sidebar, button_h, TRUE);

    // main column
    const int left = pad + sidebar + pad;
    const int main_w = width - left - pad;
    const int transcript_h = height - input_h - status_h - pad * 3 - scaled(6, dpi_);
    MoveWindow(transcript_, left, pad, main_w, transcript_h > 0 ? transcript_h : scaled(100, dpi_),
               TRUE);

    const int input_y = pad + (transcript_h > 0 ? transcript_h : scaled(100, dpi_)) + pad;
    const int send_w = scaled(88, dpi_);
    MoveWindow(input_, left, input_y, main_w - send_w - scaled(8, dpi_), input_h, TRUE);
    MoveWindow(send_, left + main_w - send_w, input_y, send_w, scaled(38, dpi_), TRUE);
    MoveWindow(stop_, left + main_w - send_w, input_y + scaled(44, dpi_), send_w,
               scaled(32, dpi_), TRUE);
    MoveWindow(status_, left, input_y + input_h + scaled(4, dpi_), main_w, status_h, TRUE);
}

void MainWindow::set_status(const std::wstring& text) { set_text(status_, text); }

// ------------------------------------------------------------------- auth

void MainWindow::apply_auth_result(const AuthResult& result, const std::wstring& fallback) {
    std::wstring text = widen(result.message);
    if (text.empty()) text = fallback;

    switch (result.status) {
        case AuthResult::Status::Ok:
            user_ = result.user;
            remember_session(result);
            show_chat();
            return;

        case AuthResult::Status::NeedsVerification:
            challenge_id_ = result.challenge_id;
            if (result.code_in_outbox) {
                text += L"\r\n\r\nEmail is not configured, so the code was saved to the "
                        L"data\\outbox folder instead of being sent. Open the newest .eml "
                        L"file there to read it.";
            }
            auth_mode_ = AuthMode::Verify;
            refresh_auth_visibility();
            set_text(auth_code_, L"");
            SetFocus(auth_code_);
            break;

        default:
            break;
    }
    set_text(auth_message_, text);
}

// ------------------------------------------------------- keep me signed in

void MainWindow::remember_session(const AuthResult& result) {
    if (!ctx_.config.remember_enabled()) return;
    if (!remember_me_ || result.session_token.empty()) {
        // Unticking the box is also how a user turns the feature back off.
        forget_session();
        return;
    }

    // The email is stored purely so the sign-in box can be pre-filled if the
    // token later turns out to be expired.
    json payload;
    payload["v"] = 1;
    payload["token"] = result.session_token;
    payload["email"] = result.user.email;

    std::string error;
    if (!credentials::save(remember_path_, payload.dump(), &error)) {
        // Not fatal: the user is signed in, they will just have to do it again
        // next time.
        std::fprintf(stderr, "[warn] could not save the sign-in: %s\n", error.c_str());
    }
}

bool MainWindow::try_restore_session() {
    if (!ctx_.config.remember_enabled()) return false;
    if (!credentials::exists(remember_path_)) return false;

    std::string blob;
    if (!credentials::load(remember_path_, &blob, nullptr)) {
        // Written by a different Windows account, or corrupt. Nothing to do
        // but discard it.
        credentials::clear(remember_path_);
        return false;
    }

    std::string token, email;
    try {
        json parsed = json::parse(blob);
        token = parsed.value("token", "");
        email = parsed.value("email", "");
    } catch (const json::exception&) {
        credentials::clear(remember_path_);
        return false;
    }

    AuthResult restored = ctx_.auth.restore_session(token);
    if (!restored.ok()) {
        credentials::clear(remember_path_);
        if (!email.empty()) {
            set_text(auth_email_, widen(email));
            set_text(auth_message_, widen(restored.message));
        }
        return false;
    }

    // The token was rotated, so the stored copy must be replaced or the next
    // start would present a revoked one.
    remember_me_ = true;
    user_ = restored.user;
    remember_session(restored);
    return true;
}

void MainWindow::forget_session() {
    credentials::clear(remember_path_);
}

void MainWindow::do_auth_primary() {
    EnableWindow(auth_primary_, FALSE);
    set_text(auth_message_, L"Working...");
    UpdateWindow(auth_message_);
    remember_me_ =
        SendMessageW(auth_remember_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (auth_mode_ == AuthMode::Login) {
        AuthResult result = ctx_.auth.log_in(narrow(get_text(auth_email_)),
                                             narrow(get_text(auth_password_)));
        set_text(auth_password_, L"");
        apply_auth_result(result, L"Sign in failed.");
    } else if (auth_mode_ == AuthMode::SignUp) {
        AuthResult result =
            ctx_.auth.sign_up(narrow(get_text(auth_email_)), narrow(get_text(auth_name_)),
                              narrow(get_text(auth_password_)));
        apply_auth_result(result, L"Sign up failed.");
    } else {
        AuthResult result =
            ctx_.auth.verify_code(challenge_id_, narrow(get_text(auth_code_)));
        apply_auth_result(result, L"That code was not accepted.");
    }

    EnableWindow(auth_primary_, TRUE);
}

void MainWindow::do_auth_switch() {
    if (auth_mode_ == AuthMode::Login) {
        auth_mode_ = AuthMode::SignUp;
    } else {
        auth_mode_ = AuthMode::Login;
    }
    set_text(auth_message_, L"");
    refresh_auth_visibility();
}

void MainWindow::do_resend() {
    AuthResult result = ctx_.auth.resend_code(challenge_id_);
    apply_auth_result(result, L"Could not send a new code.");
}

// ------------------------------------------------------------------ chats

void MainWindow::load_chats() {
    chats_ = ctx_.chats.list_chats(user_.id);
    SendMessageW(chat_list_, LB_RESETCONTENT, 0, 0);
    int select = -1;
    for (size_t i = 0; i < chats_.size(); ++i) {
        SendMessageW(chat_list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(widen(chats_[i].title).c_str()));
        if (chats_[i].id == active_chat_) select = static_cast<int>(i);
    }
    if (select >= 0) SendMessageW(chat_list_, LB_SETCURSEL, select, 0);
}

void MainWindow::open_selected_chat() {
    const int index = static_cast<int>(SendMessageW(chat_list_, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(chats_.size())) return;
    active_chat_ = chats_[index].id;
    render_chat();
}

void MainWindow::render_chat() {
    clear(transcript_);

    if (active_chat_ == 0) {
        // U+2022 BULLET, written as its explicit UTF-8 bytes and concatenated as
        // a separate literal. Two reasons:
        //   * The source file stays pure ASCII, so no editor or tool can
        //     re-encode it. A literal bullet here previously survived a
        //     round-trip through a tool that assumed Windows-1252 and became
        //     the mojibake "a-euro-cent".
        //   * Keeping it a separate literal stops the hex escape from swallowing
        //     a following character: "\xE2\x80\xA2A" would parse \xA2A as one
        //     escape, because hex escapes consume every hex digit they can.
        // append_status() converts from UTF-8 with MultiByteToWideChar.
        static const char* kBullet = "\xE2\x80\xA2";
        const std::string bullet = std::string("  ") + kBullet + "  ";
        append_status(transcript_,
                      "New chat.\n\nAsk a maths or physics question and I will work through it "
                      "step by step.\n\nFor example:\n" +
                          bullet + "Solve 3x - 7 = 14 and check the answer\n" +
                          bullet + "What is 2/3 + 3/4?\n" +
                          bullet + "Find the area of a triangle with base 12 cm and height 5 cm\n" +
                          bullet + "A car goes from 0 to 24 m/s in 6 s. What is its acceleration?\n" +
                          bullet + "Why is a bulb dimmer in series than in parallel?\n\n" +
                          "Press F1 for keyboard shortcuts.",
                      palette_, 10);
        scroll_to_end(transcript_);
        return;
    }

    for (const chat::StoredMessage& m : ctx_.chats.get_messages(user_.id, active_chat_)) {
        append_turn(transcript_, m.role, m.content, m.sources, m.off_topic, palette_, 10);
    }
    scroll_to_end(transcript_);
}

void MainWindow::new_chat() {
    active_chat_ = 0;
    SendMessageW(chat_list_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    render_chat();
    SetFocus(input_);
}

void MainWindow::rename_chat() {
    const int index = static_cast<int>(SendMessageW(chat_list_, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(chats_.size())) {
        message_box(hwnd_, L"Select a chat in the list first.", kAppTitle, MB_ICONINFORMATION);
        return;
    }
    std::wstring title = widen(chats_[index].title);
    if (!prompt_text(hwnd_, font_, L"Rename chat", L"New name for this chat:", &title)) return;

    std::string error;
    if (!ctx_.chats.rename_chat(user_.id, chats_[index].id, narrow(title), &error)) {
        message_box(hwnd_, widen(error), kAppTitle, MB_ICONWARNING);
        return;
    }
    load_chats();
}

void MainWindow::delete_chat() {
    const int index = static_cast<int>(SendMessageW(chat_list_, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(chats_.size())) {
        message_box(hwnd_, L"Select a chat in the list first.", kAppTitle, MB_ICONINFORMATION);
        return;
    }
    const std::wstring question =
        L"Delete \"" + widen(chats_[index].title) + L"\"?\r\n\r\nThis cannot be undone.";
    if (MessageBoxW(hwnd_, question.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    const int64_t id = chats_[index].id;
    ctx_.chats.delete_chat(user_.id, id);
    if (active_chat_ == id) active_chat_ = 0;
    load_chats();
    render_chat();
}

void MainWindow::show_account() {
    std::wstring info;
    info += L"Signed in as " + widen(user_.display_name) + L"\r\n";
    info += widen(user_.email) + L"\r\n\r\n";
    info += L"Member since " + widen(util::format_unix(user_.created_at)) + L" UTC\r\n";
    info += L"Sessions expire " + std::to_wstring(ctx_.config.session_days()) +
            L" days after sign in.\r\n\r\n";
    info += L"Yes\t- sign out\r\nNo\t- delete account permanently\r\nCancel\t- close";

    const int choice =
        MessageBoxW(hwnd_, info.c_str(), L"Account", MB_YESNOCANCEL | MB_ICONINFORMATION);

    if (choice == IDYES) {
        // The session token is not held in the UI; revoking every session for
        // this account is both simpler and stricter.
        ctx_.auth.log_out_all_sessions(user_.id);
        // Signing out has to clear the stored token too, or the next start
        // would silently sign back in.
        forget_session();
        remember_me_ = false;
        user_ = User{};
        chats_.clear();
        active_chat_ = 0;
        set_text(auth_message_, L"Signed out.");
        show_auth(AuthMode::Login);
        return;
    }

    if (choice != IDNO) return;

    if (MessageBoxW(hwnd_,
                    L"This permanently deletes your account and every chat you have saved.\r\n\r\n"
                    L"We will email you a code to confirm. Continue?",
                    L"Delete account", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }

    AuthResult request = ctx_.auth.request_account_deletion(user_.id);
    if (request.status != AuthResult::Status::NeedsVerification) {
        message_box(hwnd_, widen(request.message), kAppTitle, MB_ICONWARNING);
        return;
    }

    std::wstring label = L"Enter the confirmation code sent to " + widen(user_.email) + L":";
    if (request.code_in_outbox) {
        label += L"\r\n(Email is not configured - look in data\\outbox for the newest .eml)";
    }
    std::wstring code;
    if (!prompt_text(hwnd_, font_, L"Confirm deletion", label, &code)) return;

    AuthResult confirmed =
        ctx_.auth.confirm_account_deletion(user_.id, request.challenge_id, narrow(code));
    if (!confirmed.ok()) {
        message_box(hwnd_, widen(confirmed.message), kAppTitle, MB_ICONWARNING);
        return;
    }

    message_box(hwnd_, widen(confirmed.message), kAppTitle, MB_ICONINFORMATION);
    forget_session();
    remember_me_ = false;
    user_ = User{};
    chats_.clear();
    active_chat_ = 0;
    show_auth(AuthMode::Login);
}

// ------------------------------------------------------------- generation

void MainWindow::send_question() {
    if (generating_) return;

    const std::string question = util::trim(narrow(get_text(input_)));
    if (question.empty()) return;

    if (!runtime_ready_ && !ctx_.client.is_available(nullptr)) {
        message_box(hwnd_,
                    L"The tutor model is still starting up. Give it a moment and try again.",
                    kAppTitle, MB_ICONINFORMATION);
        return;
    }

    set_text(input_, L"");
    append_turn(transcript_, "user", question, "", false, palette_, 10);

    stream_anchor_ = text_length(transcript_);
    stream_started_ = false;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        stream_text_.clear();
        reasoning_chars_ = 0;
    }
    append_status(transcript_, "Working it out...", palette_, 10);
    scroll_to_end(transcript_);

    generating_ = true;
    cancel_ = false;
    EnableWindow(send_, FALSE);
    // Swapping the model out from under a running generation would kill it.
    EnableWindow(preset_, FALSE);
    ShowWindow(stop_, SW_SHOW);
    set_status(L"Thinking...");

    if (worker_.joinable()) worker_.join();
    pending_question_ = question;

    HWND target = hwnd_;
    worker_ = std::thread([this, target]() {
        auto on_token = [this, target](const std::string& piece) -> bool {
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                stream_text_ += piece;
            }
            PostMessageW(target, WM_APP_STREAM, 0, 0);
            return !cancel_;
        };
        auto on_thinking = [this, target](const std::string& piece) -> bool {
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                reasoning_chars_ += piece.size();
            }
            PostMessageW(target, WM_APP_STREAM, 1, 0);
            return !cancel_;
        };

        chat::AskResult result;
        try {
            result = ctx_.chats.ask(user_, active_chat_, pending_question_, on_token, on_thinking);
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = std::string("Internal error: ") + e.what();
        }
        last_result_ = result;
        PostMessageW(target, WM_APP_DONE, 0, 0);
    });
}

void MainWindow::stop_generation() {
    if (!generating_) return;
    cancel_ = true;
    set_status(L"Stopping...");
}

void MainWindow::on_stream_update() {
    std::string text;
    size_t reasoning = 0;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        text = stream_text_;
        reasoning = reasoning_chars_;
    }

    // Nothing to show yet: the model is still reasoning. Report progress so the
    // window does not look frozen -- on a CPU-only machine this phase can run
    // for minutes before the first word of the answer appears.
    if (text.empty()) {
        if (reasoning > 0) {
            truncate_from(transcript_, stream_anchor_);
            const std::string dots(1 + (reasoning / 200) % 3, '.');
            append_status(transcript_, "Working it out" + dots, palette_, 10);
            scroll_to_end(transcript_);
        }
        return;
    }

    // Re-render the reply in place. Rendering a few kilobytes is cheap, and it
    // keeps the markdown correct as the text grows instead of showing raw
    // asterisks until the end.
    truncate_from(transcript_, stream_anchor_);
    stream_started_ = true;
    append_turn(transcript_, "assistant", text, "", false, palette_, 10);
    scroll_to_end(transcript_);
    set_status(L"Writing the answer...");
}

void MainWindow::on_generation_done() {
    if (worker_.joinable()) worker_.join();
    generating_ = false;
    EnableWindow(send_, TRUE);
    set_preset_enabled(true);
    ShowWindow(stop_, SW_HIDE);

    truncate_from(transcript_, stream_anchor_);

    const chat::AskResult& result = last_result_;
    if (result.ok) {
        std::string sources_json;
        if (!result.sources.empty()) {
            sources_json = "[";
            for (size_t i = 0; i < result.sources.size(); ++i) {
                if (i) sources_json += ",";
                sources_json += "{\"source\":\"" + result.sources[i].source + "\",\"locator\":\"" +
                                result.sources[i].locator + "\"}";
            }
            sources_json += "]";
        }
        append_turn(transcript_, "assistant", result.answer, sources_json, result.off_topic,
                    palette_, 10);
        active_chat_ = result.chat_id;
        load_chats();
        set_status(L"");
    } else {
        append_status(transcript_, result.error, palette_, 10);
        if (result.chat_id != 0) {
            active_chat_ = result.chat_id;
            load_chats();
        }
        set_status(L"Something went wrong.");
    }

    scroll_to_end(transcript_);
    SetFocus(input_);
}

// ------------------------------------------------------------- keyboard

void MainWindow::build_accelerators() {
    // Combinations follow Microsoft's "Common Keyboard Accelerators" table so
    // they match what users already know from other Windows applications:
    // Ctrl+N adds an item, F2 renames one, Ctrl+D deletes the selection,
    // F6 moves to the next pane, Ctrl+W closes, and F1 is help.
    //
    // FVIRTKEY is required for the key field to be a virtual-key code; without
    // it the entry is interpreted as an ASCII character and the modifiers are
    // ignored.
    const ACCEL table[] = {
        {FVIRTKEY | FCONTROL, 'N', kIdNewChat},
        {FVIRTKEY,            VK_F2, kIdRenameChat},
        {FVIRTKEY | FCONTROL, 'D', kIdDeleteChat},
        {FVIRTKEY | FCONTROL, 'L', kIdAccelFocusInput},
        {FVIRTKEY,            VK_F6, kIdAccelNextPane},
        {FVIRTKEY | FSHIFT,   VK_F6, kIdAccelPrevPane},
        {FVIRTKEY,            VK_F1, kIdAccelShortcuts},
        {FVIRTKEY | FCONTROL, '1', kIdAccelPresetFast},
        {FVIRTKEY | FCONTROL, '2', kIdAccelPresetSmart},
        // Ctrl+Tab is the conventional "next document" and does not collide
        // with Tab, which IsDialogMessage uses for focus movement.
        {FVIRTKEY | FCONTROL,            VK_TAB, kIdAccelNextChat},
        {FVIRTKEY | FCONTROL | FSHIFT,   VK_TAB, kIdAccelPrevChat},
        {FVIRTKEY | FCONTROL, 'W', kIdAccelClose},
        {FVIRTKEY | FCONTROL, VK_RETURN, kIdSend},
    };

    accelerators_ = CreateAcceleratorTableW(const_cast<ACCEL*>(table),
                                            static_cast<int>(std::size(table)));
    if (!accelerators_) {
        std::fprintf(stderr, "[warn] keyboard shortcuts unavailable (%lu)\n", GetLastError());
    }
}

void MainWindow::focus_input() {
    if (!IsWindowVisible(input_)) return;
    SetFocus(input_);
    // Put the caret at the end rather than selecting everything, so Ctrl+L
    // during a half-typed question does not risk replacing it.
    const int length = GetWindowTextLengthW(input_);
    SendMessageW(input_, EM_SETSEL, length, length);
}

void MainWindow::cycle_pane(bool forward) {
    // F6 moves between the three regions a user actually works in. The buttons
    // stay reachable with Tab; this is the coarse jump.
    HWND panes[] = {chat_list_, input_, transcript_};
    const int count = static_cast<int>(std::size(panes));

    HWND focus = GetFocus();
    int current = -1;
    for (int i = 0; i < count; ++i) {
        // A button inside the sidebar counts as being in the list pane.
        if (focus == panes[i]) current = i;
    }
    if (current < 0) current = forward ? count - 1 : 0;

    for (int step = 1; step <= count; ++step) {
        const int next = ((current + (forward ? step : count - step)) % count + count) % count;
        if (IsWindowVisible(panes[next]) && IsWindowEnabled(panes[next])) {
            SetFocus(panes[next]);
            return;
        }
    }
}

void MainWindow::select_chat_relative(int delta) {
    const int count = static_cast<int>(SendMessageW(chat_list_, LB_GETCOUNT, 0, 0));
    if (count <= 0) return;
    int index = static_cast<int>(SendMessageW(chat_list_, LB_GETCURSEL, 0, 0));
    if (index < 0) index = delta > 0 ? -1 : 0;
    index = ((index + delta) % count + count) % count;  // wrap in both directions
    SendMessageW(chat_list_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    open_selected_chat();
}

void MainWindow::show_shortcuts() {
    // Kept as a plain message box: it is a reference card, and a dedicated
    // window would be one more thing to make keyboard accessible.
    message_box(hwnd_,
                L"Keyboard shortcuts\n"
                L"\n"
                L"Moving around\n"
                L"  Tab / Shift+Tab\tNext / previous control\n"
                L"  F6 / Shift+F6\tNext / previous pane\n"
                L"  Ctrl+L\t\tGo to the message box\n"
                L"  Alt+<letter>\tAny underlined letter\n"
                L"\n"
                L"Chats\n"
                L"  Ctrl+N\t\tNew chat\n"
                L"  F2\t\tRename the selected chat\n"
                L"  Ctrl+D\t\tDelete the selected chat\n"
                L"  Ctrl+Tab\t\tNext chat\n"
                L"  Ctrl+Shift+Tab\tPrevious chat\n"
                L"  Up / Down\t\tMove through the chat list\n"
                L"  Enter\t\tOpen the highlighted chat\n"
                L"  Delete\t\tDelete the highlighted chat\n"
                L"\n"
                L"Asking a question\n"
                L"  Enter\t\tSend\n"
                L"  Shift+Enter\tNew line\n"
                L"  Ctrl+Enter\tSend from anywhere\n"
                L"  Esc\t\tStop generating\n"
                L"\n"
                L"Editing text\n"
                L"  Ctrl+Backspace\tDelete the word to the left\n"
                L"  Ctrl+Delete\tDelete the word to the right\n"
                L"  Ctrl+A / C / V / X / Z\tSelect all, copy, paste, cut, undo\n"
                L"\n"
                L"Answer style\n"
                L"  Ctrl+1\t\tFast\n"
                L"  Ctrl+2\t\tSmart\n"
                L"\n"
                L"Other\n"
                L"  F1\t\tThis list\n"
                L"  Ctrl+W\t\tClose the window",
                L"Keyboard shortcuts", MB_ICONINFORMATION);
}

// ---------------------------------------------------------- answer style

void MainWindow::populate_presets() {
    SendMessageW(preset_, CB_RESETCONTENT, 0, 0);

    const std::vector<runtime::Preset>& presets = ctx_.runtime.presets();
    for (const runtime::Preset& p : presets) {
        SendMessageW(preset_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(widen(p.label).c_str()));
    }

    // Prefer this user's remembered choice over whatever booted.
    std::string wanted = prefs::get(ctx_.database, user_.id, prefs::kModelPreset,
                                    ctx_.runtime.active_preset());
    bool known = false;
    for (const runtime::Preset& p : presets) known = known || p.id == wanted;
    if (!known) wanted = ctx_.runtime.active_preset();

    int index = 0;
    for (size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].id == wanted) index = static_cast<int>(i);
    }
    SendMessageW(preset_, CB_SETCURSEL, index, 0);
    last_preset_index_ = index;

    // Loading a different model costs a full restart, so it is deferred until
    // the user actually asks for it rather than done at sign-in.
    if (wanted != ctx_.runtime.active_preset()) on_preset_changed();
}

void MainWindow::set_preset_enabled(bool enabled) {
    EnableWindow(preset_, enabled && !generating_ && !switching_);
}

void MainWindow::on_preset_changed() {
    if (switching_ || generating_) {
        // Cannot swap the model mid-answer; put the box back.
        SendMessageW(preset_, CB_SETCURSEL, last_preset_index_, 0);
        return;
    }

    const int index = static_cast<int>(SendMessageW(preset_, CB_GETCURSEL, 0, 0));
    const std::vector<runtime::Preset>& presets = ctx_.runtime.presets();
    if (index < 0 || index >= static_cast<int>(presets.size())) return;
    if (presets[index].id == ctx_.runtime.active_preset()) {
        last_preset_index_ = index;
        return;
    }

    requested_preset_ = presets[index].id;
    switching_ = true;
    EnableWindow(preset_, FALSE);
    EnableWindow(send_, FALSE);
    set_status(L"Switching to " + widen(presets[index].label) + L"...");

    if (switch_thread_.joinable()) switch_thread_.join();

    HWND target = hwnd_;
    const std::string preset_id = requested_preset_;
    switch_thread_ = std::thread([this, target, preset_id]() {
        std::string error;
        const bool ok = ctx_.runtime.switch_preset(
            preset_id,
            [this, target](const std::string& message) {
                {
                    std::lock_guard<std::mutex> lock(runtime_mutex_);
                    runtime_message_ = widen(message);
                }
                PostMessageW(target, WM_APP_RUNTIME, 0, 0);
            },
            &error);
        switch_error_ = ok ? std::string() : error;
        PostMessageW(target, WM_APP_SWITCHED, ok ? 1 : 0, 0);
    });
}

void MainWindow::on_preset_switched() {
    if (switch_thread_.joinable()) switch_thread_.join();
    switching_ = false;

    const runtime::Status& st = ctx_.runtime.status();
    ctx_.client.set_chat_url(st.chat_url);

    const std::vector<runtime::Preset>& presets = ctx_.runtime.presets();
    const std::string active = ctx_.runtime.active_preset();
    for (size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].id == active) last_preset_index_ = static_cast<int>(i);
    }
    SendMessageW(preset_, CB_SETCURSEL, last_preset_index_, 0);

    EnableWindow(preset_, TRUE);
    EnableWindow(send_, TRUE);

    if (switch_error_.empty()) {
        // Remember it for this account, and app-wide so the next launch boots
        // straight into the same model.
        prefs::set(ctx_.database, user_.id, prefs::kModelPreset, active);
        prefs::set_app(ctx_.database, prefs::kLastModelPreset, active);
        std::wstring label = widen(active);
        for (const runtime::Preset& p : presets) {
            if (p.id == active) label = widen(p.label);
        }
        set_status(L"Ready \x2013 " + label + L" (" + widen(ctx_.runtime.chat_model_name()) + L")");
    } else {
        set_status(L"Could not switch: " + widen(switch_error_));
    }
}

// --------------------------------------------------------------- runtime

void MainWindow::start_runtime() {
    if (!ctx_.runtime.available()) {
        // No bundled weights: fall back to whatever the config points at.
        std::string error;
        if (ctx_.client.is_available(&error)) {
            runtime_ready_ = true;
            set_status(L"Using the configured model server.");
        } else {
            set_status(L"No model available. See README.md.");
        }
        return;
    }

    // Nobody has signed in yet, so the per-user choice is unknown. The app-wide
    // echo of the last selection gets the single-user case right, which avoids
    // loading one model and immediately swapping it for another.
    const std::string boot_preset =
        prefs::get_app(ctx_.database, prefs::kLastModelPreset, ctx_.config.preset_default());

    HWND target = hwnd_;
    runtime_thread_ = std::thread([this, target, boot_preset]() {
        std::string error;
        const bool ok = ctx_.runtime.start(
            [this, target](const std::string& message) {
                {
                    std::lock_guard<std::mutex> lock(runtime_mutex_);
                    runtime_message_ = widen(message);
                }
                PostMessageW(target, WM_APP_RUNTIME, 0, 0);
            },
            &error, boot_preset);

        {
            std::lock_guard<std::mutex> lock(runtime_mutex_);
            runtime_message_ = ok ? L"" : widen(error);
        }
        PostMessageW(target, WM_APP_READY, ok ? 1 : 0, 0);
    });
}

// ------------------------------------------------------------ window proc

LRESULT MainWindow::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            hwnd_ = hwnd;
            dpi_ = GetDpiForWindow(hwnd);
            remember_path_ =
                (std::filesystem::path(ctx_.config.db_path()).parent_path() / "remember.bin")
                    .string();
            build_controls();
            build_accelerators();
            // Restore before showing anything, so a remembered user never sees
            // the sign-in screen flash past.
            if (try_restore_session()) {
                show_chat();
            } else {
                show_auth(AuthMode::Login);
            }
            start_runtime();
            return 0;
        }

        case WM_SIZE:
            layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = scaled(760, dpi_);
            info->ptMinTrackSize.y = scaled(520, dpi_);
            return 0;
        }

        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wparam), &rc, background_brush_);
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, reinterpret_cast<HWND>(lparam) == auth_message_ ? palette_.muted
                                                                             : palette_.text);
            return reinterpret_cast<LRESULT>(background_brush_);
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.surface);
            return reinterpret_cast<LRESULT>(surface_brush_);
        }

        // IsDialogMessage asks the window which button Enter should press. A
        // plain window returns nothing, so Enter did nothing at all on the
        // sign-in screen unless a button already had focus. Answering this is
        // what makes Enter submit the form from any field.
        case DM_GETDEFID:
            return MAKELRESULT(user_.id == 0 ? kIdAuthPrimary : kIdSend, DC_HASDEFID);

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);

            // IsDialogMessage translates Enter to IDOK and Escape to IDCANCEL
            // when no button has focus. Map them to the action for the screen
            // in view.
            if (id == IDOK) {
                if (user_.id == 0) do_auth_primary(); else send_question();
                return 0;
            }
            if (id == IDCANCEL) {
                // Escape stops a running answer; it must never close the
                // window, which is the default dialog behaviour.
                if (generating_) stop_generation();
                return 0;
            }

            // A command arrives either from a control (BN_CLICKED, which is 0)
            // or from the accelerator table, which sets the notification code
            // to 1. Both must run the same action, or every shortcut would be
            // silently ignored.
            const bool invoked = (code == BN_CLICKED || code == 1);

            // Chat commands are meaningless on the sign-in screen; ignoring
            // them there stops a stray shortcut from acting on no account.
            const bool signed_in = user_.id != 0;

            if (id == kIdAuthPrimary && invoked) { do_auth_primary(); return 0; }
            if (id == kIdAuthSwitch && invoked) { do_auth_switch(); return 0; }
            if (id == kIdAuthResend && invoked) { do_resend(); return 0; }

            if (id == kIdNewChat && invoked && signed_in) { new_chat(); return 0; }
            if (id == kIdRenameChat && invoked && signed_in) { rename_chat(); return 0; }
            if (id == kIdDeleteChat && invoked && signed_in) { delete_chat(); return 0; }
            if (id == kIdAccount && invoked && signed_in) { show_account(); return 0; }
            if (id == kIdSend && invoked && signed_in) { send_question(); return 0; }
            if (id == kIdStop && invoked && signed_in) { stop_generation(); return 0; }
            if (id == kIdChatList && code == LBN_SELCHANGE) { open_selected_chat(); return 0; }
            if (id == kIdPreset && code == CBN_SELCHANGE) { on_preset_changed(); return 0; }

            // --- accelerator-only commands ---------------------------------
            if (id == kIdAccelShortcuts) { show_shortcuts(); return 0; }
            if (id == kIdAccelClose) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (!signed_in) break;
            if (id == kIdAccelFocusInput) { focus_input(); return 0; }
            if (id == kIdAccelNextPane) { cycle_pane(true); return 0; }
            if (id == kIdAccelPrevPane) { cycle_pane(false); return 0; }
            if (id == kIdAccelNextChat) { select_chat_relative(1); return 0; }
            if (id == kIdAccelPrevChat) { select_chat_relative(-1); return 0; }
            if (id == kIdAccelPresetFast || id == kIdAccelPresetSmart) {
                const std::string wanted = (id == kIdAccelPresetFast) ? "fast" : "smart";
                const std::vector<runtime::Preset>& presets = ctx_.runtime.presets();
                for (size_t i = 0; i < presets.size(); ++i) {
                    if (presets[i].id != wanted) continue;
                    SendMessageW(preset_, CB_SETCURSEL, i, 0);
                    on_preset_changed();
                    break;
                }
                return 0;
            }
            break;
        }

        case WM_APP_STREAM:
            on_stream_update();
            return 0;

        case WM_APP_DONE:
            on_generation_done();
            return 0;

        case WM_APP_SWITCHED:
            on_preset_switched();
            return 0;

        case WM_APP_RUNTIME: {
            std::lock_guard<std::mutex> lock(runtime_mutex_);
            set_status(runtime_message_);
            return 0;
        }

        case WM_APP_READY: {
            std::wstring error;
            {
                std::lock_guard<std::mutex> lock(runtime_mutex_);
                error = runtime_message_;
            }
            if (wparam == 1) {
                runtime_ready_ = true;
                const runtime::Status& st = ctx_.runtime.status();
                ctx_.client.set_chat_url(st.chat_url);
                if (st.embed_running) ctx_.client.set_embed_url(st.embed_url);

                std::wstring label;
                for (const runtime::Preset& p : ctx_.runtime.presets()) {
                    if (p.id == ctx_.runtime.active_preset()) label = widen(p.label);
                }
                set_status(L"Ready \x2013 " + (label.empty() ? L"" : label + L" \x2013 ") +
                           widen(ctx_.runtime.chat_model_name()) + L" on " +
                           widen(st.accelerator));
            } else {
                set_status(L"Model failed to start: " + error);
            }
            return 0;
        }

        case WM_CLOSE:
            if (generating_) {
                if (MessageBoxW(hwnd, L"An answer is still being written. Close anyway?",
                                kAppTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return 0;
                }
                cancel_ = true;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            // Unblock the worker before joining, or closing mid-answer would
            // hang until generation finished.
            cancel_ = true;
            if (worker_.joinable()) worker_.join();
            if (runtime_thread_.joinable()) runtime_thread_.join();
            // A model swap in flight has to finish before its thread can be
            // joined; it is bounded by the start-up timeout.
            if (switch_thread_.joinable()) switch_thread_.join();
            if (accelerators_) DestroyAcceleratorTable(accelerators_);
            if (font_) DeleteObject(font_);
            if (font_bold_) DeleteObject(font_bold_);
            if (background_brush_) DeleteObject(background_brush_);
            if (surface_brush_) DeleteObject(surface_brush_);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int run(AppContext& context) {
    auto window = std::make_unique<MainWindow>(context);
    if (!window->create()) return 1;
    return window->run_message_loop();
}

}  // namespace gui
