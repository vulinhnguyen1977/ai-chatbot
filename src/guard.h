// guard.h -- keeps the assistant inside middle school maths and physics.
//
// Two stages, cheapest first:
//   1. A lexical gate that resolves the clear cases with no model call at all:
//      obvious subject vocabulary, obvious off-domain vocabulary, greetings,
//      and prompt-injection attempts.
//   2. For anything still ambiguous, a constrained-JSON classification call to
//      the local model.
// The tutor's own system prompt is the third layer, so a classifier mistake
// still does not produce an off-topic answer.
#pragma once

#include <string>
#include <vector>

#include "llm.h"

class Config;

namespace guard {

enum class Verdict {
    Math,       // in scope
    Physics,    // in scope
    FollowUp,   // in scope, only meaningful with prior context
    SmallTalk,  // greeting or capability question -- answer briefly, then steer
    OffTopic    // refuse
};

struct Decision {
    Verdict verdict = Verdict::OffTopic;
    // True when stage 1 settled it, i.e. no model call was made.
    bool decided_lexically = false;
    std::string reason;

    bool in_scope() const {
        return verdict == Verdict::Math || verdict == Verdict::Physics ||
               verdict == Verdict::FollowUp;
    }
};

class DomainGuard {
public:
    DomainGuard(llm::Client& client, const Config& config);

    // `has_history` tells the guard whether a bare follow-up like "why?" can
    // legitimately refer to something.
    Decision classify(const std::string& message, bool has_history);

    // Stage 1 only. Exposed for testing.
    Decision lexical_stage(const std::string& message, bool has_history) const;

private:
    llm::Client& client_;
    const Config& config_;
    bool use_model_ = true;
    std::string model_;
};

}  // namespace guard
