#include "gui_richtext.h"

#include <richedit.h>

#include <vector>

#include "json.hpp"
#include "markdown.h"
#include "util.h"

using json = nlohmann::json;

namespace gui {
namespace {

constexpr int kTwipsPerPoint = 20;
// RichEdit measures indentation in twips; 1 level ~ 0.15 inch reads well.
constexpr long kIndentStep = 220;

CHARFORMAT2W base_format(const Palette& palette, int point_size) {
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_BACKCOLOR;
    wcscpy_s(cf.szFaceName, L"Segoe UI");
    cf.yHeight = point_size * kTwipsPerPoint;
    cf.crTextColor = palette.text;
    cf.crBackColor = palette.background;
    cf.dwEffects = 0;
    return cf;
}

void set_paragraph(HWND rich_edit, long indent_twips, long space_before, long space_after) {
    PARAFORMAT2 pf{};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_STARTINDENT | PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_OFFSET;
    pf.dxStartIndent = indent_twips;
    pf.dxOffset = 0;
    pf.dySpaceBefore = space_before;
    pf.dySpaceAfter = space_after;
    SendMessageW(rich_edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
}

void append(HWND rich_edit, const std::wstring& text, const CHARFORMAT2W& format) {
    // Collapse the caret to the very end so the insert always lands there,
    // whatever the user has selected.
    const long end = text_length(rich_edit);
    CHARRANGE at_end{end, end};
    SendMessageW(rich_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at_end));
    SendMessageW(rich_edit, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));
    SendMessageW(rich_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void render_blocks(HWND rich_edit, const std::vector<md::Block>& blocks, const Palette& palette,
                   int point_size) {
    for (const md::Block& block : blocks) {
        CHARFORMAT2W cf = base_format(palette, point_size);
        long indent = 0;
        std::wstring prefix;

        switch (block.type) {
            case md::BlockType::Heading:
                cf.yHeight = (point_size + (block.level <= 2 ? 2 : 1)) * kTwipsPerPoint;
                cf.crTextColor = palette.heading;
                cf.dwEffects |= CFE_BOLD;
                set_paragraph(rich_edit, 0, 120, 40);
                break;

            case md::BlockType::Bullet:
                indent = kIndentStep * (block.indent + 1);
                prefix = L"\x2022  ";  // bullet
                set_paragraph(rich_edit, indent, 0, 40);
                break;

            case md::BlockType::Numbered:
                indent = kIndentStep * (block.indent + 1);
                prefix = widen(block.marker) + L"  ";
                set_paragraph(rich_edit, indent, 0, 40);
                break;

            case md::BlockType::CodeBlock:
                cf.crBackColor = palette.code_background;
                wcscpy_s(cf.szFaceName, L"Consolas");
                set_paragraph(rich_edit, kIndentStep, 60, 60);
                break;

            case md::BlockType::Quote:
                cf.crTextColor = palette.muted;
                cf.dwEffects |= CFE_ITALIC;
                indent = kIndentStep;
                set_paragraph(rich_edit, indent, 40, 40);
                break;

            case md::BlockType::Rule:
                cf.crTextColor = palette.border;
                set_paragraph(rich_edit, 0, 60, 60);
                append(rich_edit, L"\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\n", cf);
                continue;

            case md::BlockType::TableRow:
                set_paragraph(rich_edit, kIndentStep, 0, 20);
                break;

            case md::BlockType::Paragraph:
            default:
                set_paragraph(rich_edit, 0, 0, 60);
                break;
        }

        if (!prefix.empty()) {
            CHARFORMAT2W marker = cf;
            marker.crTextColor = palette.muted;
            append(rich_edit, prefix, marker);
        }

        for (const md::Span& span : block.spans) {
            CHARFORMAT2W run = cf;
            if (span.bold) run.dwEffects |= CFE_BOLD;
            if (span.italic) run.dwEffects |= CFE_ITALIC;
            if (span.code && block.type != md::BlockType::CodeBlock) {
                wcscpy_s(run.szFaceName, L"Consolas");
                run.crBackColor = palette.code_background;
            }
            append(rich_edit, widen(span.text), run);
        }
        append(rich_edit, L"\n", cf);
    }
}

}  // namespace

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

Palette palette_for_system() {
    // Follow the Windows app theme. A missing value means light, which is the
    // default on a fresh install.
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);

    Palette p{};
    if (light == 0) {
        p.dark = true;
        p.background = RGB(0x1B, 0x1D, 0x21);
        p.text = RGB(0xE6, 0xE8, 0xEB);
        p.muted = RGB(0x98, 0x9F, 0xA9);
        p.heading = RGB(0xFF, 0xFF, 0xFF);
        p.accent = RGB(0x6E, 0x9B, 0xFF);
        p.code_background = RGB(0x2A, 0x2E, 0x34);
        p.surface = RGB(0x22, 0x25, 0x2A);
        p.border = RGB(0x3A, 0x3F, 0x46);
        p.danger = RGB(0xE8, 0x6B, 0x60);
    } else {
        p.dark = false;
        p.background = RGB(0xFF, 0xFF, 0xFF);
        p.text = RGB(0x1A, 0x1C, 0x21);
        p.muted = RGB(0x62, 0x69, 0x74);
        p.heading = RGB(0x0D, 0x0F, 0x14);
        p.accent = RGB(0x1F, 0x5C, 0xD8);
        p.code_background = RGB(0xF1, 0xF3, 0xF6);
        p.surface = RGB(0xF6, 0xF7, 0xF9);
        p.border = RGB(0xDD, 0xE1, 0xE6);
        p.danger = RGB(0xC0, 0x30, 0x28);
    }
    return p;
}

long text_length(HWND rich_edit) {
    // Returns the index at which the next character would be inserted.
    //
    // Not WM_GETTEXTLENGTH or EM_GETTEXTLENGTHEX: those count in a different
    // index space from EM_EXSETSEL (a line break is CRLF in one and a single
    // character in the other), so mixing them leaves stale text behind when the
    // streaming reply is re-rendered.
    //
    // Nor "select all and read cpMax": that range includes the final paragraph
    // mark, so it overshoots the insertion point by one and leaves a single
    // stray character behind.
    //
    // Placing the caret past the end lets the control clamp it to the real end;
    // reading it back gives exactly the insertion index, in the same units the
    // selection API uses everywhere else.
    CHARRANGE past_end{LONG_MAX, LONG_MAX};
    SendMessageW(rich_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&past_end));
    CHARRANGE clamped{};
    SendMessageW(rich_edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&clamped));
    return clamped.cpMin;
}

void truncate_from(HWND rich_edit, long from) {
    if (from < 0) return;
    // cpMax of -1 means "to the very end", so no length has to be computed and
    // nothing can be left behind.
    CHARRANGE tail{from, -1};
    SendMessageW(rich_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
    SendMessageW(rich_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}

void clear(HWND rich_edit) {
    CHARRANGE all{0, -1};
    SendMessageW(rich_edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&all));
    SendMessageW(rich_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}

void scroll_to_end(HWND rich_edit) {
    SendMessageW(rich_edit, WM_VSCROLL, SB_BOTTOM, 0);
}

void append_status(HWND rich_edit, const std::string& text, const Palette& palette,
                   int base_point_size) {
    CHARFORMAT2W cf = base_format(palette, base_point_size);
    cf.crTextColor = palette.muted;
    cf.dwEffects |= CFE_ITALIC;
    set_paragraph(rich_edit, 0, 0, 60);
    append(rich_edit, widen(text) + L"\n", cf);
}

void append_turn(HWND rich_edit, const std::string& role, const std::string& markdown,
                 const std::string& sources, bool off_topic, const Palette& palette,
                 int base_point_size) {
    const bool is_user = role == "user";

    // --- speaker label ----------------------------------------------------
    CHARFORMAT2W label = base_format(palette, base_point_size - 1);
    label.crTextColor = is_user ? palette.accent : palette.muted;
    label.dwEffects |= CFE_BOLD;
    set_paragraph(rich_edit, 0, 200, 40);
    append(rich_edit, is_user ? L"You\n" : L"Tutor\n", label);

    // --- body -------------------------------------------------------------
    if (is_user) {
        // The student's own words are shown verbatim: treating their input as
        // markdown would let a stray asterisk silently change what they typed.
        CHARFORMAT2W cf = base_format(palette, base_point_size);
        set_paragraph(rich_edit, 0, 0, 60);
        append(rich_edit, widen(markdown) + L"\n", cf);
    } else {
        render_blocks(rich_edit, md::parse(markdown), palette, base_point_size);
    }

    if (off_topic) {
        CHARFORMAT2W note = base_format(palette, base_point_size - 1);
        note.crTextColor = palette.muted;
        note.dwEffects |= CFE_ITALIC;
        set_paragraph(rich_edit, 0, 20, 40);
        append(rich_edit, L"(outside maths and physics)\n", note);
    }

    // --- citations ---------------------------------------------------------
    if (!sources.empty()) {
        std::string line;
        try {
            json parsed = json::parse(sources);
            std::vector<std::string> seen;
            for (const auto& item : parsed) {
                std::string name = item.value("source", "");
                const std::string locator = item.value("locator", "");
                if (name.empty()) continue;
                if (!locator.empty()) name += ", " + locator;
                bool duplicate = false;
                for (const std::string& s : seen) duplicate = duplicate || s == name;
                if (duplicate) continue;
                seen.push_back(name);
                if (!line.empty()) line += "  -  ";
                line += name;
            }
        } catch (const json::exception&) {
            line.clear();
        }
        if (!line.empty()) {
            CHARFORMAT2W cite = base_format(palette, base_point_size - 1);
            cite.crTextColor = palette.muted;
            set_paragraph(rich_edit, 0, 20, 60);
            append(rich_edit, L"Sources: " + widen(line) + L"\n", cite);
        }
    }
}

}  // namespace gui
