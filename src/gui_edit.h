// gui_edit.h -- keyboard fixes for the standard Win32 EDIT control.
//
// The EDIT control does not implement Ctrl+Backspace or Ctrl+Delete. Windows
// translates Ctrl+Backspace into WM_CHAR with a character code of 0x7F (DEL),
// and because the control has no handler for it, it inserts that as a literal
// character -- so pressing Ctrl+Backspace *adds* an invisible character instead
// of deleting the previous word. This is long-standing documented behaviour,
// not a bug in this application; the .NET team tracks the same issue for
// WinForms TextBox (dotnet/winforms#259).
//
// The fix is to subclass the control, handle the two key combinations, and
// swallow the stray control characters. Subclassing uses SetWindowSubclass /
// DefSubclassProc from ComCtl32 v6, which Microsoft documents as the supported
// approach (it supports removal and per-instance reference data, unlike
// replacing the window procedure with SetWindowLongPtr).
#pragma once

#include <cstddef>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace gui {

// Returns the offset the caret should move back to when deleting the word to
// the left of `caret`, following the convention used by Windows text fields:
// skip any whitespace immediately before the caret, then consume one run of
// either word characters or punctuation.
//
// Pure function so the behaviour can be tested without creating a window.
size_t word_boundary_left(const std::wstring& text, size_t caret);

// The mirror image, for Ctrl+Delete: the offset up to which text should be
// removed forward of `caret`.
size_t word_boundary_right(const std::wstring& text, size_t caret);

// Subclasses an EDIT control so that Ctrl+Backspace and Ctrl+Delete delete a
// word instead of inserting a control character. Safe to call more than once on
// the same control. The subclass is removed automatically when the control is
// destroyed.
void install_edit_key_fixes(HWND edit);

}  // namespace gui
