#include "guard.h"

#include <cctype>

#include "config.h"
#include "json.hpp"
#include "prompts.h"
#include "util.h"

using json = nlohmann::json;

namespace guard {
namespace {

// Vocabulary that is unambiguously school mathematics.
const char* const kMathTerms[] = {
    "fraction", "fractions", "decimal", "decimals", "percent", "percentage", "percentages",
    "ratio", "ratios", "proportion", "proportions", "integer", "integers", "exponent",
    "exponents", "square root", "cube root", "algebra", "equation", "equations", "inequality",
    "inequalities", "expression", "simplify", "factorise", "factorize", "factor", "expand",
    "solve for", "quadratic", "linear", "coordinate", "coordinates", "graph", "gradient",
    "slope", "intercept", "geometry", "triangle", "triangles", "quadrilateral", "rectangle",
    "square", "circle", "circles", "polygon", "angle", "angles", "parallel", "perpendicular",
    "congruent", "similar", "perimeter", "area", "volume", "surface area", "circumference",
    "radius", "diameter", "pythagoras", "pythagorean", "hypotenuse", "mean", "median", "mode",
    "range", "average", "probability", "statistics", "histogram", "bar chart", "pie chart",
    "prime", "factorisation", "factorization", "multiple", "divisor", "lcm", "hcf", "gcd",
    "numerator", "denominator", "arithmetic", "multiply", "divide", "subtract", "addition",
    "subtraction", "multiplication", "division", "rounding", "estimate", "bodmas", "pemdas",
    "sequence", "nth term", "scale factor", "symmetry", "transformation", "rotation",
    "reflection", "translation", "vertex", "midpoint", "maths", "math", "mathematics"};

// Vocabulary that is unambiguously school physics.
const char* const kPhysicsTerms[] = {
    "physics", "velocity", "acceleration", "speed", "displacement", "momentum", "newton",
    "newtons", "force", "forces", "friction", "gravity", "gravitational", "weight", "mass",
    "inertia", "work done", "power", "joule", "joules", "watt", "watts", "energy", "kinetic",
    "potential energy", "conservation of energy", "machine", "lever", "pulley", "inclined plane",
    "density", "pressure", "pascal", "buoyancy", "upthrust", "floating", "heat", "temperature",
    "thermal", "celsius", "kelvin", "conduction", "convection", "radiation", "wave", "waves",
    "wavelength", "frequency", "amplitude", "hertz", "sound", "echo", "vibration", "light",
    "reflection", "refraction", "lens", "mirror", "prism", "spectrum", "magnet", "magnetic",
    "magnetism", "electric", "electricity", "circuit", "circuits", "current", "voltage",
    "resistance", "ohm", "ohms", "ampere", "amps", "volt", "volts", "resistor", "series",
    "battery", "conductor", "insulator", "static electricity", "free fall", "terminal velocity",
    "motion", "distance-time", "speed-time", "velocity-time",
    // Falling shows up constantly in gravity questions phrased without jargon
    // ("why does a heavier object not fall faster?").
    "fall", "falls", "falling", "fell", "accelerate", "accelerates", "decelerate"};

// Topics that are firmly outside the product, listed so they can be refused
// with no model call. Science neighbours are included on purpose: chemistry
// and biology are the most common near-miss requests.
const char* const kOffTopicTerms[] = {
    "chemistry", "chemical", "periodic table", "molecule", "molecules", "compound",
    "biology", "cell", "cells", "dna", "genetics", "evolution", "photosynthesis", "organism",
    "anatomy", "history", "historical", "war", "civilisation", "civilization", "geography",
    "capital", "country", "countries", "continent", "population of", "flag of", "language of",
    "president", "prime minister", "election", "politics", "political",
    "government", "religion", "philosophy", "economics", "stock", "crypto", "bitcoin",
    "investment", "recipe", "cook", "cooking", "restaurant", "movie", "film", "song",
    "lyrics", "celebrity", "football", "basketball", "video game", "minecraft", "roblox",
    "world cup", "olympics", "tournament", "athlete", "premier league", "nba", "fifa",
    "python", "javascript", "java", "c++", "html", "css", "sql", "programming", "code for",
    "write a program", "debug", "medical", "medicine", "diagnose", "symptom", "disease",
    "doctor", "therapy", "depressed", "anxiety", "dating", "girlfriend", "boyfriend",
    "relationship advice", "essay about", "poem", "story about", "novel", "translate",
    "spanish", "french", "grammar", "resume", "cv", "cover letter", "weather forecast",
    "news", "joke", "riddle"};

// Attempts to talk the assistant out of its role.
const char* const kInjectionTerms[] = {
    "ignore your instructions", "ignore previous instructions", "ignore all previous",
    "disregard your instructions", "forget your instructions", "forget everything",
    "you are no longer", "you are now", "pretend to be", "pretend you are", "act as if you",
    "roleplay as", "role play as", "jailbreak", "developer mode", "system prompt",
    "your instructions", "without restrictions", "no restrictions", "bypass your"};

// Greetings and capability questions.
const char* const kSmallTalkPhrases[] = {
    "hello", "hi", "hey", "good morning", "good afternoon", "good evening", "how are you",
    "who are you", "what are you", "what can you do", "what do you do", "help me", "thanks",
    "thank you", "thankyou", "cheers", "bye", "goodbye", "ok", "okay", "cool", "nice"};

// Short messages that only make sense as continuations.
const char* const kFollowUpPhrases[] = {
    "why", "why?", "how", "how so", "explain", "explain that", "explain more", "more detail",
    "i don't get it", "i dont get it", "i don't understand", "i dont understand", "again",
    "another", "another one", "one more", "next", "continue", "go on", "show me", "prove it",
    "is that right", "are you sure", "check it", "simplify that", "in metres", "in meters",
    "step 1", "step 2", "step 3", "the last step", "that step", "what about"};

template <size_t N>
bool any_term(const std::string& lower, const char* const (&terms)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (util::contains_word(lower, terms[i])) return true;
    }
    return false;
}

template <size_t N>
bool any_substring(const std::string& lower, const char* const (&terms)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (util::contains(lower, terms[i])) return true;
    }
    return false;
}

template <size_t N>
bool equals_any(const std::string& lower, const char* const (&terms)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (lower == terms[i]) return true;
    }
    return false;
}

// True for text that reads like a calculation: digits combined with operators,
// or an equation. "2x + 5 = 11" and "12 * 7" both qualify.
bool looks_like_arithmetic(const std::string& s) {
    bool has_digit = false, has_operator = false, has_equals = false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) has_digit = true;
        if (c == '+' || c == '*' || c == '/' || c == '^' || c == '%') has_operator = true;
        if (c == '=') has_equals = true;
    }
    // A lone '-' is too common in ordinary prose to count as an operator.
    return has_digit && (has_operator || has_equals);
}

std::string strip_trailing_punctuation(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == '?' || s[end - 1] == '!' || s[end - 1] == '.' ||
                       s[end - 1] == ' ')) {
        --end;
    }
    return s.substr(0, end);
}

}  // namespace

DomainGuard::DomainGuard(llm::Client& client, const Config& config)
    : client_(client), config_(config) {
    use_model_ = config.guard_use_model();
    model_ = config.guard_model_name();
}

Decision DomainGuard::lexical_stage(const std::string& message, bool has_history) const {
    Decision d;
    d.decided_lexically = true;

    const std::string trimmed = util::trim(message);
    const std::string lower = util::to_lower(trimmed);
    const std::string bare = strip_trailing_punctuation(lower);

    if (trimmed.empty()) {
        d.verdict = Verdict::SmallTalk;
        d.reason = "empty message";
        return d;
    }

    // Injection attempts are refused before anything else, so a message that
    // pairs "ignore your instructions" with a maths word cannot slip through.
    if (any_substring(lower, kInjectionTerms)) {
        d.verdict = Verdict::OffTopic;
        d.reason = "instruction override attempt";
        return d;
    }

    const bool math = any_term(lower, kMathTerms);
    const bool physics = any_term(lower, kPhysicsTerms);
    const bool off = any_term(lower, kOffTopicTerms);

    // Off-domain vocabulary wins over subject vocabulary: "write python code to
    // solve this equation" is a programming request, not a maths question.
    if (off) {
        d.verdict = Verdict::OffTopic;
        d.reason = "off-domain subject vocabulary";
        return d;
    }

    if (math && !physics) {
        d.verdict = Verdict::Math;
        d.reason = "mathematics vocabulary";
        return d;
    }
    if (physics && !math) {
        d.verdict = Verdict::Physics;
        d.reason = "physics vocabulary";
        return d;
    }
    if (math && physics) {
        // Mixed vocabulary is normal in physics problems ("area" + "pressure").
        d.verdict = Verdict::Physics;
        d.reason = "mixed vocabulary, treated as physics";
        return d;
    }

    if (equals_any(bare, kSmallTalkPhrases)) {
        d.verdict = Verdict::SmallTalk;
        d.reason = "greeting";
        return d;
    }

    if (has_history && (equals_any(bare, kFollowUpPhrases) || bare.size() <= 24)) {
        d.verdict = Verdict::FollowUp;
        d.reason = "short continuation of an existing chat";
        return d;
    }

    if (looks_like_arithmetic(trimmed)) {
        d.verdict = Verdict::Math;
        d.reason = "expression contains a calculation";
        return d;
    }

    // Nothing conclusive -- stage 2 has to decide.
    d.decided_lexically = false;
    d.reason = "ambiguous";
    return d;
}

Decision DomainGuard::classify(const std::string& message, bool has_history) {
    Decision d = lexical_stage(message, has_history);
    if (d.decided_lexically) return d;

    if (!use_model_) {
        // Without the classifier the safe default is to refuse: answering an
        // off-topic question is the failure mode this product must not have.
        d.verdict = Verdict::OffTopic;
        d.reason = "ambiguous and model classification disabled";
        return d;
    }

    std::vector<llm::Message> messages = {
        {"system", prompts::kClassifierSystem},
        {"user", util::utf8_truncate(message, 4000)}};

    llm::ChatOptions options;
    options.model = model_;
    options.temperature = 0.0;
    options.num_ctx = 2048;
    // Enough headroom for the schema-constrained object even if the model is
    // verbose about it; the classifier reply is a single short field.
    options.num_predict = 200;
    options.format = prompts::kClassifierSchema;
    // Reasoning here is wasted latency: the answer is one enum value, and on a
    // thinking model the reasoning tokens would consume num_predict before any
    // JSON appeared.
    options.think = "false";
    options.timeout_s = 60;

    llm::ChatResult result = client_.chat(messages, options);
    if (!result.ok) {
        // The model is unreachable. Refusing here would make an Ollama outage
        // look like a content decision, so let it through and rely on the
        // tutor system prompt, which also refuses off-topic questions.
        d.verdict = has_history ? Verdict::FollowUp : Verdict::Math;
        d.reason = "classifier unavailable, deferring to the tutor prompt";
        return d;
    }

    std::string category;
    try {
        json parsed = json::parse(result.content);
        category = util::to_lower(parsed.value("category", ""));
    } catch (const json::exception&) {
        // Structured output should make this unreachable; fall back to a
        // substring probe on the raw reply.
        const std::string lower = util::to_lower(result.content);
        for (const char* c : {"math", "physics", "followup", "smalltalk", "other"}) {
            if (util::contains(lower, c)) {
                category = c;
                break;
            }
        }
    }

    d.decided_lexically = false;
    d.reason = "model classified as '" + category + "'";
    if (category == "math") {
        d.verdict = Verdict::Math;
    } else if (category == "physics") {
        d.verdict = Verdict::Physics;
    } else if (category == "followup") {
        d.verdict = has_history ? Verdict::FollowUp : Verdict::OffTopic;
    } else if (category == "smalltalk") {
        // Genuine small talk is short. A long message classified this way is
        // far more likely to be an off-domain question the model mislabelled
        // ("What is the capital of France?"), and answering it with a cheery
        // greeting misreports why we are not helping. Length is a blunt but
        // reliable tiebreaker, and erring towards the refusal is the safe
        // direction for this product.
        const size_t length = util::trim(message).size();
        if (length <= 60) {
            d.verdict = Verdict::SmallTalk;
        } else {
            d.verdict = Verdict::OffTopic;
            d.reason += " (overridden to off-topic: too long for small talk)";
        }
    } else {
        d.verdict = Verdict::OffTopic;
    }
    return d;
}

}  // namespace guard
