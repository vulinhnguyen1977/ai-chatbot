// markdown.h -- turns model output into styled text for the desktop UI.
//
// Two stages, and the first is the one that matters most in practice:
//
//   normalise_math()  Models trained on maths corpora emit LaTeX no matter how
//                     firmly the prompt forbids it: "$$12 \times 7$$",
//                     "\frac{3}{4}", "\(x^2\)". Rendered literally that is the
//                     unreadable mess of dollar signs and backslashes a student
//                     sees. This converts the common constructs to the plain
//                     notation a middle school student actually reads, and
//                     strips the delimiters.
//
//   parse()           Splits the cleaned text into blocks (headings, lists,
//                     code, quotes, tables) whose inline spans carry bold /
//                     italic / code flags. The renderer applies real character
//                     formatting rather than printing the '*' characters.
#pragma once

#include <string>
#include <vector>

namespace md {

enum class BlockType {
    Paragraph,
    Heading,
    Bullet,
    Numbered,
    CodeBlock,
    Quote,
    Rule,
    TableRow,
};

// A run of text sharing one set of character attributes.
struct Span {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool code = false;
};

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;        // heading depth (1-6)
    int indent = 0;       // nesting depth for list items
    std::string marker;   // "1." / "2." for numbered items
    bool table_header = false;
    std::vector<Span> spans;
};

// Converts LaTeX and other maths notation to plain readable text. Safe to run
// on text that contains no maths at all.
std::string normalise_math(const std::string& text);

// Full pipeline: normalise, then parse into blocks.
std::vector<Block> parse(const std::string& text);

// Parses text that has already been normalised.
std::vector<Block> parse_normalised(const std::string& text);

// Flattens blocks back to readable plain text. Used for the clipboard and for
// the self-tests.
std::string to_plain_text(const std::vector<Block>& blocks);

}  // namespace md
