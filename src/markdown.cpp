#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "util.h"

namespace md {
namespace {

// ------------------------------------------------------------ maths clean-up

struct Replacement {
    const char* from;
    const char* to;
};

// Symbol commands. Longer names must come before their own prefixes ("\\leq"
// before "\\le") or the shorter one wins and leaves a stray character.
const Replacement kSymbols[] = {
    {"\\approx", "~="},   {"\\neq", "!="},      {"\\leqslant", "<="}, {"\\geqslant", ">="},
    {"\\leq", "<="},      {"\\geq", ">="},      {"\\le", "<="},       {"\\ge", ">="},
    // \cdot is multiplication at this level. Rendering it as "." would be read
    // as a decimal point, so it becomes the same "x" as \times.
    {"\\times", " x "},   {"\\cdot", " x "},    {"\\div", " / "},     {"\\pm", "+/-"},
    {"\\degree", " degrees"}, {"\\circ", ""},   {"\\percent", "%"},
    {"\\rightarrow", " -> "}, {"\\Rightarrow", " => "}, {"\\to", " -> "},
    {"\\leftarrow", " <- "},  {"\\ldots", "..."},      {"\\dots", "..."},
    {"\\infty", "infinity"},
    {"\\alpha", "alpha"}, {"\\beta", "beta"},   {"\\gamma", "gamma"},
    {"\\theta", "theta"}, {"\\lambda", "lambda"}, {"\\mu", "mu"},
    {"\\pi", "pi"},       {"\\rho", "rho"},     {"\\sigma", "sigma"},
    {"\\omega", "omega"}, {"\\Delta", "change in "}, {"\\delta", "delta"},
    {"\\quad", "  "},     {"\\qquad", "    "},
    {"\\left", ""},       {"\\right", ""},
    {"\\!", ""},          {"\\,", " "},         {"\\;", " "},        {"\\:", " "},
};

// Commands of the form \name{...} whose braces are simply dropped.
const char* const kTransparentCommands[] = {
    "\\text", "\\textrm", "\\textbf", "\\textit", "\\mathrm", "\\mathbf", "\\mathit",
    "\\operatorname", "\\displaystyle", "\\mbox",
    // \boxed{84} is how these models like to present a final answer.
    "\\boxed",
};

bool starts_with_at(const std::string& s, size_t pos, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= pos + n && s.compare(pos, n, prefix) == 0;
}

// Reads a {...} group starting at `pos` (which must index the '{').
// Returns the contents and advances `pos` past the closing brace.
bool read_group(const std::string& s, size_t& pos, std::string* out) {
    if (pos >= s.size() || s[pos] != '{') return false;
    int depth = 0;
    size_t i = pos;
    for (; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}') {
            --depth;
            if (depth == 0) break;
        }
    }
    if (i >= s.size()) return false;  // unbalanced; leave it alone
    *out = s.substr(pos + 1, i - pos - 1);
    pos = i + 1;
    return true;
}

std::string convert_commands(const std::string& in, int depth);

// \frac{a}{b} -> a/b, parenthesising compound numerators or denominators so
// "\frac{x+1}{2}" becomes "(x+1)/2" rather than the wrong "x+1/2".
bool needs_parens(const std::string& s) {
    if (s.size() <= 1) return false;
    for (char c : s) {
        if (c == '+' || c == '-' || c == ' ' || c == '/') return true;
    }
    return false;
}

std::string convert_commands(const std::string& in, int depth) {
    // Nested fractions are legitimate but bounded; the cap stops a pathological
    // input from recursing without end.
    if (depth > 8) return in;

    std::string out;
    out.reserve(in.size());
    size_t i = 0;

    while (i < in.size()) {
        if (in[i] != '\\') {
            out.push_back(in[i++]);
            continue;
        }

        // --- fractions ---------------------------------------------------
        bool handled = false;
        for (const char* frac : {"\\dfrac", "\\tfrac", "\\frac"}) {
            if (!starts_with_at(in, i, frac)) continue;
            size_t p = i + std::strlen(frac);
            while (p < in.size() && in[p] == ' ') ++p;
            std::string num, den;
            if (read_group(in, p, &num) && p < in.size()) {
                while (p < in.size() && in[p] == ' ') ++p;
                if (read_group(in, p, &den)) {
                    std::string a = convert_commands(num, depth + 1);
                    std::string b = convert_commands(den, depth + 1);
                    out += needs_parens(a) ? "(" + a + ")" : a;
                    out += "/";
                    out += needs_parens(b) ? "(" + b + ")" : b;
                    i = p;
                    handled = true;
                }
            }
            break;
        }
        if (handled) continue;

        // --- roots ---------------------------------------------------------
        if (starts_with_at(in, i, "\\sqrt")) {
            size_t p = i + 5;
            while (p < in.size() && in[p] == ' ') ++p;
            std::string arg;
            if (read_group(in, p, &arg)) {
                out += "sqrt(" + convert_commands(arg, depth + 1) + ")";
                i = p;
                continue;
            }
        }

        // --- commands whose braces just disappear --------------------------
        for (const char* cmd : kTransparentCommands) {
            if (!starts_with_at(in, i, cmd)) continue;
            size_t p = i + std::strlen(cmd);
            std::string arg;
            if (read_group(in, p, &arg)) {
                out += convert_commands(arg, depth + 1);
                i = p;
                handled = true;
            }
            break;
        }
        if (handled) continue;

        // --- plain symbol commands -----------------------------------------
        for (const Replacement& r : kSymbols) {
            if (!starts_with_at(in, i, r.from)) continue;
            // Do not let "\\pi" swallow the "\\pit" of a longer unknown command.
            const size_t after = i + std::strlen(r.from);
            const bool boundary =
                after >= in.size() || !std::isalpha(static_cast<unsigned char>(in[after]));
            if (!boundary) continue;
            out += r.to;
            i = after;
            handled = true;
            break;
        }
        if (handled) continue;

        // --- escaped literals ----------------------------------------------
        if (i + 1 < in.size()) {
            const char next = in[i + 1];
            if (next == '%' || next == '$' || next == '&' || next == '#' || next == '_' ||
                next == '{' || next == '}') {
                out.push_back(next);
                i += 2;
                continue;
            }
            if (next == '\\') {  // LaTeX line break
                out.push_back('\n');
                i += 2;
                continue;
            }
        }

        // Unknown command: drop the backslash, keep the letters. Better to show
        // "beta" than "\beta".
        out.push_back(' ');
        ++i;
        while (i < in.size() && std::isalpha(static_cast<unsigned char>(in[i]))) {
            out.push_back(in[i++]);
        }
    }
    return out;
}

// ^{12} -> ^12 and _{1} -> _1. Single-character groups lose the braces too.
std::string simplify_scripts(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if ((in[i] == '^' || in[i] == '_') && i + 1 < in.size() && in[i + 1] == '{') {
            size_t p = i + 1;
            std::string arg;
            if (read_group(in, p, &arg)) {
                out.push_back(in[i]);
                out += arg;
                i = p;
                continue;
            }
        }
        out.push_back(in[i++]);
    }
    return out;
}

// Marks a markdown hard line break (a line ending in two or more spaces).
//
// The signal has to be preserved through tidy_spaces, which would otherwise
// collapse it away. These models end almost every line this way, and dropping
// them ran the "What we know" quantities together into one sentence.
constexpr char kHardBreak = '\x01';

std::string mark_hard_breaks(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\n') {
            out.push_back(in[i]);
            continue;
        }
        // Count the spaces immediately before this newline.
        size_t spaces = 0;
        while (spaces < out.size() && out[out.size() - 1 - spaces] == ' ') ++spaces;
        if (spaces >= 2) {
            out.resize(out.size() - spaces);
            out.push_back(kHardBreak);
        }
        out.push_back('\n');
    }
    return out;
}

// Collapses runs of spaces but never touches newlines, so block structure
// survives into the parser.
std::string tidy_spaces(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool last_space = false;
    for (char c : in) {
        if (c == ' ' || c == '\t') {
            if (!last_space) out.push_back(' ');
            last_space = true;
        } else {
            out.push_back(c);
            last_space = false;
        }
    }
    // Drop spaces that ended up before punctuation or a line end.
    std::string tidy;
    tidy.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == ' ' && i + 1 < out.size() &&
            (out[i + 1] == '\n' || out[i + 1] == ',' || out[i + 1] == '.' ||
             out[i + 1] == ')' || out[i + 1] == ';')) {
            continue;
        }
        tidy.push_back(out[i]);
    }
    return tidy;
}

// ------------------------------------------------------------ inline parsing

void push_span(std::vector<Span>* spans, const std::string& text, bool bold, bool italic,
               bool code) {
    if (text.empty()) return;
    // Merge with the previous span when the attributes match, so the renderer
    // makes one formatting call instead of many.
    if (!spans->empty()) {
        Span& last = spans->back();
        if (last.bold == bold && last.italic == italic && last.code == code) {
            last.text += text;
            return;
        }
    }
    Span s;
    s.text = text;
    s.bold = bold;
    s.italic = italic;
    s.code = code;
    spans->push_back(std::move(s));
}

bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

std::vector<Span> parse_inline(const std::string& line) {
    std::vector<Span> spans;
    std::string buffer;
    bool bold = false, italic = false;
    size_t i = 0;

    auto flush = [&]() {
        push_span(&spans, buffer, bold, italic, false);
        buffer.clear();
    };

    while (i < line.size()) {
        const char c = line[i];

        // `code`
        if (c == '`') {
            size_t end = line.find('`', i + 1);
            if (end != std::string::npos && end > i + 1) {
                flush();
                push_span(&spans, line.substr(i + 1, end - i - 1), bold, italic, true);
                i = end + 1;
                continue;
            }
        }

        // ***both***  **bold**  __bold__
        if ((c == '*' || c == '_') && i + 2 < line.size() && line[i + 1] == c &&
            line[i + 2] == c) {
            flush();
            bold = !bold;
            italic = !italic;
            i += 3;
            continue;
        }
        if ((c == '*' || c == '_') && i + 1 < line.size() && line[i + 1] == c) {
            flush();
            bold = !bold;
            i += 2;
            continue;
        }

        // *italic* / _italic_. An underscore between word characters is part of
        // an identifier (v_final), not emphasis.
        if (c == '*' || c == '_') {
            const bool inner_underscore =
                c == '_' && i > 0 && is_word_char(line[i - 1]) &&
                i + 1 < line.size() && is_word_char(line[i + 1]);
            if (!inner_underscore) {
                // Only treat it as emphasis when a closing mark exists.
                if (italic || line.find(c, i + 1) != std::string::npos) {
                    flush();
                    italic = !italic;
                    ++i;
                    continue;
                }
            }
        }

        buffer.push_back(c);
        ++i;
    }
    flush();
    return spans;
}

std::string strip_trailing(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\r' || s[end - 1] == '\t')) --end;
    return s.substr(0, end);
}

int leading_spaces(const std::string& s) {
    int n = 0;
    while (n < static_cast<int>(s.size()) && (s[n] == ' ' || s[n] == '\t')) {
        n += (s[n] == '\t') ? 4 : 1;
    }
    return n;
}

bool is_rule(const std::string& t) {
    if (t.size() < 3) return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char ch : t) {
        if (ch != c && ch != ' ') return false;
    }
    return true;
}

// "| --- | :--: |" -- the separator under a table header.
bool is_table_separator(const std::string& t) {
    if (t.find('|') == std::string::npos) return false;
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    }
    return t.find('-') != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------- public API

std::string normalise_math(const std::string& text) {
    std::string s = text;

    // Display maths: $$...$$ and \[...\] become their own line.
    std::string stage;
    stage.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '$') {
            size_t end = s.find("$$", i + 2);
            if (end != std::string::npos) {
                stage += "\n";
                stage += s.substr(i + 2, end - i - 2);
                stage += "\n";
                i = end + 2;
                continue;
            }
        }
        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == '[') {
            size_t end = s.find("\\]", i + 2);
            if (end != std::string::npos) {
                stage += "\n";
                stage += s.substr(i + 2, end - i - 2);
                stage += "\n";
                i = end + 2;
                continue;
            }
        }
        // Inline maths: \(...\)
        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == '(') {
            size_t end = s.find("\\)", i + 2);
            if (end != std::string::npos) {
                stage += s.substr(i + 2, end - i - 2);
                i = end + 2;
                continue;
            }
        }
        // Inline maths: $...$, but only when it looks like maths rather than
        // two separate currency amounts ("costs $5 and $3 more").
        if (s[i] == '$' && (i == 0 || s[i - 1] != '\\')) {
            size_t end = s.find('$', i + 1);
            if (end != std::string::npos && end > i + 1 && end - i < 200) {
                const std::string inner = s.substr(i + 1, end - i - 1);
                const bool has_newline = inner.find('\n') != std::string::npos;
                const bool looks_like_money =
                    !inner.empty() && std::isdigit(static_cast<unsigned char>(inner[0])) &&
                    inner.find('\\') == std::string::npos &&
                    inner.find('^') == std::string::npos &&
                    inner.find('=') == std::string::npos;
                if (!has_newline && !looks_like_money) {
                    stage += inner;
                    i = end + 1;
                    continue;
                }
            }
        }
        stage.push_back(s[i]);
        ++i;
    }
    s = stage;

    // Environments (\begin{aligned} ... \end{aligned}) -- keep the body.
    for (const char* keyword : {"\\begin", "\\end"}) {
        size_t pos = 0;
        while ((pos = s.find(keyword, pos)) != std::string::npos) {
            size_t p = pos + std::strlen(keyword);
            std::string name;
            if (p < s.size() && s[p] == '{' && read_group(s, p, &name)) {
                s.erase(pos, p - pos);
            } else {
                pos = p;
            }
        }
    }

    s = convert_commands(s, 0);
    s = simplify_scripts(s);
    s = mark_hard_breaks(s);
    s = tidy_spaces(s);
    return s;
}

std::vector<Block> parse_normalised(const std::string& text) {
    std::vector<Block> blocks;
    const std::vector<std::string> lines = util::split(text, '\n');

    bool in_code = false;
    std::string code_accumulator;
    // Consecutive plain lines join into one paragraph, but a blank line ends
    // it. Without this every section of an answer runs into the next one.
    bool previous_was_blank = true;
    // A line that is entirely bold ("**What we know**") is a section title in
    // the tutor's answer format, not a sentence, so nothing may merge into it.
    bool previous_was_title = false;

    // Set when the previous line ended in a markdown hard break, so the next
    // line continues the same block but starts on a new visual line.
    bool previous_hard_break = false;

    for (size_t index = 0; index < lines.size(); ++index) {
        std::string raw = strip_trailing(lines[index]);

        const bool hard_break = !raw.empty() && raw.back() == kHardBreak;
        if (hard_break) raw.pop_back();
        // Any stray marker elsewhere must never reach the screen.
        raw.erase(std::remove(raw.begin(), raw.end(), kHardBreak), raw.end());

        const std::string trimmed = util::trim(raw);

        // ``` fenced code
        if (util::starts_with(trimmed, "```")) {
            if (in_code) {
                Block b;
                b.type = BlockType::CodeBlock;
                push_span(&b.spans, code_accumulator, false, false, true);
                blocks.push_back(std::move(b));
                code_accumulator.clear();
                in_code = false;
            } else {
                in_code = true;
            }
            continue;
        }
        if (in_code) {
            if (!code_accumulator.empty()) code_accumulator += "\n";
            code_accumulator += raw;
            continue;
        }

        if (trimmed.empty()) {
            previous_was_blank = true;
            previous_was_title = false;
            continue;
        }

        if (is_rule(trimmed)) {
            Block b;
            b.type = BlockType::Rule;
            blocks.push_back(std::move(b));
            continue;
        }

        // Tables. The separator row is structural, not content -- and it comes
        // *after* the header, so seeing it is what tells us the row already
        // emitted was the header.
        if (is_table_separator(trimmed)) {
            if (!blocks.empty() && blocks.back().type == BlockType::TableRow) {
                blocks.back().table_header = true;
                for (Span& s : blocks.back().spans) s.bold = true;
            }
            continue;
        }
        if (trimmed.find('|') != std::string::npos && trimmed.front() == '|') {
            Block b;
            b.type = BlockType::TableRow;
            std::string cells = trimmed;
            if (!cells.empty() && cells.front() == '|') cells.erase(0, 1);
            if (!cells.empty() && cells.back() == '|') cells.pop_back();
            // Render cells separated by spacing; a real grid is more than this
            // UI needs, and aligned text stays readable.
            std::string flat;
            for (const std::string& cell : util::split(cells, '|')) {
                if (!flat.empty()) flat += "   ";
                flat += util::trim(cell);
            }
            b.spans = parse_inline(flat);
            blocks.push_back(std::move(b));
            previous_was_blank = false;
            previous_was_title = false;
            continue;
        }

        // # heading
        if (trimmed[0] == '#') {
            int level = 0;
            while (level < static_cast<int>(trimmed.size()) && trimmed[level] == '#') ++level;
            if (level <= 6 && level < static_cast<int>(trimmed.size()) && trimmed[level] == ' ') {
                Block b;
                b.type = BlockType::Heading;
                b.level = level;
                b.spans = parse_inline(util::trim(trimmed.substr(level)));
                blocks.push_back(std::move(b));
                continue;
            }
        }

        // > quote
        if (trimmed[0] == '>') {
            Block b;
            b.type = BlockType::Quote;
            b.spans = parse_inline(util::trim(trimmed.substr(1)));
            blocks.push_back(std::move(b));
            continue;
        }

        const int indent = leading_spaces(raw);

        // - bullet
        if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
            trimmed[1] == ' ') {
            Block b;
            b.type = BlockType::Bullet;
            b.indent = indent / 2;
            b.spans = parse_inline(util::trim(trimmed.substr(2)));
            blocks.push_back(std::move(b));
            continue;
        }

        // 1. numbered
        {
            size_t digits = 0;
            while (digits < trimmed.size() &&
                   std::isdigit(static_cast<unsigned char>(trimmed[digits]))) {
                ++digits;
            }
            if (digits > 0 && digits + 1 < trimmed.size() &&
                (trimmed[digits] == '.' || trimmed[digits] == ')') &&
                trimmed[digits + 1] == ' ') {
                Block b;
                b.type = BlockType::Numbered;
                b.indent = indent / 2;
                b.marker = trimmed.substr(0, digits) + ".";
                b.spans = parse_inline(util::trim(trimmed.substr(digits + 2)));
                blocks.push_back(std::move(b));
                continue;
            }
        }

        // A whole-line bold run is a section title. The tutor's answer format
        // uses these ("**What we know**"), so they are promoted to headings and
        // never absorbed into a neighbouring paragraph.
        const bool whole_line_bold =
            trimmed.size() > 4 && util::starts_with(trimmed, "**") &&
            trimmed.compare(trimmed.size() - 2, 2, "**") == 0 &&
            trimmed.find("**", 2) == trimmed.size() - 2;
        if (whole_line_bold) {
            Block b;
            b.type = BlockType::Heading;
            b.level = 3;
            b.spans = parse_inline(trimmed.substr(2, trimmed.size() - 4));
            for (Span& s : b.spans) s.bold = true;
            blocks.push_back(std::move(b));
            previous_was_blank = false;
            previous_was_title = true;
            continue;
        }

        // A paragraph. Consecutive plain lines join into one block so wrapping
        // is decided by the window width, not by where the model broke lines.
        if (!blocks.empty() && blocks.back().type == BlockType::Paragraph &&
            !previous_was_blank && !previous_was_title) {
            // A hard break keeps the lines in one block but on separate lines;
            // otherwise they flow together and wrap to the window width.
            push_span(&blocks.back().spans, previous_hard_break ? "\n" : " ", false, false, false);
            std::vector<Span> more = parse_inline(trimmed);
            for (Span& s : more) push_span(&blocks.back().spans, s.text, s.bold, s.italic, s.code);
            previous_was_blank = false;
            previous_hard_break = hard_break;
            continue;
        }

        Block b;
        b.type = BlockType::Paragraph;
        b.spans = parse_inline(trimmed);
        blocks.push_back(std::move(b));
        previous_was_blank = false;
        previous_was_title = false;
        previous_hard_break = hard_break;
    }

    // An unterminated fence still has content worth showing.
    if (in_code && !code_accumulator.empty()) {
        Block b;
        b.type = BlockType::CodeBlock;
        push_span(&b.spans, code_accumulator, false, false, true);
        blocks.push_back(std::move(b));
    }

    return blocks;
}

std::vector<Block> parse(const std::string& text) {
    return parse_normalised(normalise_math(text));
}

std::string to_plain_text(const std::vector<Block>& blocks) {
    std::string out;
    for (const Block& b : blocks) {
        std::string line;
        for (const Span& s : b.spans) line += s.text;

        switch (b.type) {
            case BlockType::Heading: out += line + "\n"; break;
            case BlockType::Bullet:
                out += std::string(b.indent * 2, ' ') + "- " + line + "\n";
                break;
            case BlockType::Numbered:
                out += std::string(b.indent * 2, ' ') + b.marker + " " + line + "\n";
                break;
            case BlockType::CodeBlock: out += line + "\n"; break;
            case BlockType::Quote: out += "> " + line + "\n"; break;
            case BlockType::Rule: out += "----\n"; break;
            case BlockType::TableRow: out += line + "\n"; break;
            case BlockType::Paragraph: out += line + "\n"; break;
        }
    }
    return out;
}

}  // namespace md
