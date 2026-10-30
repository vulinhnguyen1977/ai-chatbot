#include "gui_edit.h"

#include <commctrl.h>

#include <vector>

namespace gui {
namespace {

// Subclass identity. A subclass is identified by the (procedure, id) pair, so
// this only has to be distinct from the other ids this application uses on the
// same control -- the message input already carries one for its Enter handling.
constexpr UINT_PTR kEditSubclassId = 0x45444954;  // 'EDIT'

bool is_word_char(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
           c == L'_' || static_cast<unsigned>(c) > 127;  // treat non-ASCII as word text
}

bool is_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

std::wstring get_edit_text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return buffer;
}

// Replaces the current selection, going through EM_REPLACESEL with
// canUndo = TRUE so the deletion joins the control's own undo stack and Ctrl+Z
// still works.
void replace_selection(HWND edit, const wchar_t* text) {
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text));
}

void delete_word_left(HWND edit) {
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));

    // With an active selection, Ctrl+Backspace deletes just that, matching
    // every other Windows text field.
    if (start != end) {
        replace_selection(edit, L"");
        return;
    }
    if (start == 0) return;

    const std::wstring text = get_edit_text(edit);
    const size_t target = word_boundary_left(text, start);
    if (target >= start) return;

    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(target), static_cast<LPARAM>(start));
    replace_selection(edit, L"");
}

void delete_word_right(HWND edit) {
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));

    if (start != end) {
        replace_selection(edit, L"");
        return;
    }

    const std::wstring text = get_edit_text(edit);
    if (end >= text.size()) return;

    const size_t target = word_boundary_right(text, end);
    if (target <= end) return;

    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(target));
    replace_selection(edit, L"");
}

bool control_held() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }

LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (message) {
        case WM_KEYDOWN:
            if (wparam == VK_BACK && control_held()) {
                delete_word_left(hwnd);
                return 0;
            }
            if (wparam == VK_DELETE && control_held()) {
                delete_word_right(hwnd);
                return 0;
            }
            if (wparam == VK_TAB && !control_held()) {
                // A multi-line edit answers WM_GETDLGCODE with DLGC_WANTALLKEYS,
                // which makes IsDialogMessage hand it every key -- including
                // Tab. The result is that Tab does nothing at all and the
                // keyboard cannot leave the field.
                //
                // Clearing that flag would be the obvious fix, but it would also
                // give Enter and Shift+Enter to the dialog manager and break
                // "Shift+Enter inserts a newline". So Tab is handled here
                // instead, doing exactly what the dialog manager would have.
                HWND parent = GetParent(hwnd);
                if (parent) {
                    const bool backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    HWND next = GetNextDlgTabItem(parent, hwnd, backwards);
                    if (next && next != hwnd) SetFocus(next);
                }
                return 0;
            }
            break;

        case WM_CHAR:
            // Swallow the control characters the EDIT control would otherwise
            // insert verbatim:
            //   0x7F  Ctrl+Backspace  (the reported "adds a character" bug)
            //   0x0A  Ctrl+Enter
            //   0x09  Tab, now that focus movement is handled above
            // The corresponding WM_KEYDOWN has already done the real work.
            if (wparam == 0x7F || wparam == 0x0A || wparam == 0x09) return 0;
            break;

        case WM_NCDESTROY:
            // Microsoft requires the subclass to be removed before the window
            // is gone. WM_NCDESTROY is the documented last chance.
            RemoveWindowSubclass(hwnd, edit_subclass_proc, kEditSubclassId);
            break;

        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

}  // namespace

size_t word_boundary_left(const std::wstring& text, size_t caret) {
    size_t i = caret > text.size() ? text.size() : caret;
    if (i == 0) return 0;

    // Whitespace directly before the caret goes first, so Ctrl+Backspace at the
    // end of "hello world   " removes the spaces and "world" together.
    while (i > 0 && is_space(text[i - 1])) --i;
    if (i == 0) return 0;

    if (is_word_char(text[i - 1])) {
        while (i > 0 && is_word_char(text[i - 1])) --i;
    } else {
        // A run of punctuation ("=+-") is deleted as one unit.
        while (i > 0 && !is_word_char(text[i - 1]) && !is_space(text[i - 1])) --i;
    }
    return i;
}

size_t word_boundary_right(const std::wstring& text, size_t caret) {
    const size_t n = text.size();
    size_t i = caret > n ? n : caret;
    if (i >= n) return n;

    if (is_word_char(text[i])) {
        while (i < n && is_word_char(text[i])) ++i;
    } else if (!is_space(text[i])) {
        while (i < n && !is_word_char(text[i]) && !is_space(text[i])) ++i;
    }
    // Trailing whitespace is consumed so the following word moves up to the
    // caret, which is what Ctrl+Delete does elsewhere in Windows.
    while (i < n && is_space(text[i])) ++i;
    return i;
}

void install_edit_key_fixes(HWND edit) {
    if (!edit) return;
    // SetWindowSubclass replaces an existing subclass with the same procedure
    // and id rather than stacking a second one, so repeat calls are harmless.
    SetWindowSubclass(edit, edit_subclass_proc, kEditSubclassId, 0);
}

}  // namespace gui
