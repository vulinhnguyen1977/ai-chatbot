// gui_richtext.h -- renders parsed markdown into a RichEdit control.
//
// This is where the markdown actually stops being characters and starts being
// formatting: '**' becomes a bold run rather than two asterisks on screen.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace gui {

// Colours used by the transcript, so light and dark share one code path.
struct Palette {
    COLORREF background;
    COLORREF text;
    COLORREF muted;
    COLORREF heading;
    COLORREF accent;
    COLORREF code_background;
    COLORREF surface;
    COLORREF border;
    COLORREF danger;
    bool dark;
};

Palette palette_for_system();

// UTF-8 <-> UTF-16 helpers. Win32 controls are UTF-16; everything else in the
// application is UTF-8.
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

// Appends one chat turn to the transcript, applying markdown formatting.
// `role` is "user" or "assistant". Text is markdown; LaTeX is normalised away
// before rendering.
void append_turn(HWND rich_edit, const std::string& role, const std::string& markdown,
                 const std::string& sources, bool off_topic, const Palette& palette,
                 int base_point_size);

// Appends muted italic status text (for example "Working it out...").
void append_status(HWND rich_edit, const std::string& text, const Palette& palette,
                   int base_point_size);

// Total character count, used to remember where a streaming reply begins.
long text_length(HWND rich_edit);

// Deletes everything from `from` to the end. Used to re-render the streaming
// reply in place as more of it arrives.
void truncate_from(HWND rich_edit, long from);

void clear(HWND rich_edit);
void scroll_to_end(HWND rich_edit);

}  // namespace gui
