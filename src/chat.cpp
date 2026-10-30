#include "chat.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "config.h"
#include "json.hpp"
#include "prompts.h"
#include "util.h"

using json = nlohmann::json;

namespace chat {
namespace {

constexpr size_t kMaxQuestionBytes = 8000;
constexpr size_t kMaxTitleBytes = 120;

std::string sources_to_json(const std::vector<rag::Passage>& passages) {
    if (passages.empty()) return "";
    json arr = json::array();
    for (const rag::Passage& p : passages) {
        arr.push_back({{"source", p.source},
                       {"locator", p.locator},
                       {"subject", p.subject},
                       {"score", p.score}});
    }
    return arr.dump();
}

}  // namespace

ChatService::ChatService(db::Database& database, llm::Client& client,
                         guard::DomainGuard& domain_guard, rag::Retriever& retriever,
                         const Config& config)
    : db_(database),
      client_(client),
      guard_(domain_guard),
      retriever_(retriever),
      config_(config) {
    history_turns_ = static_cast<int>(config.get_int("chat.history_turns", 8));
}

std::string ChatService::derive_title(const std::string& question) {
    std::string cleaned;
    cleaned.reserve(question.size());
    bool last_space = false;
    for (char c : question) {
        // Collapse newlines and runs of whitespace so the title stays one line.
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!last_space && !cleaned.empty()) cleaned.push_back(' ');
            last_space = true;
        } else {
            cleaned.push_back(c);
            last_space = false;
        }
    }
    cleaned = util::trim(cleaned);
    if (cleaned.empty()) return "New chat";

    std::string title = util::utf8_truncate(cleaned, 60);
    if (title.size() < cleaned.size()) {
        // Prefer cutting at the last word boundary rather than mid-word.
        size_t space = title.rfind(' ');
        if (space != std::string::npos && space > 20) title = title.substr(0, space);
        title += "...";
    }
    return title;
}

int64_t ChatService::create_chat(int64_t user_id, const std::string& title) {
    const int64_t now = util::now_unix();
    std::lock_guard<std::mutex> lock(db_.mutex());
    db::Stmt s(db_, "INSERT INTO chats(user_id, title, created_at, updated_at) VALUES(?,?,?,?)");
    s.bind(1, user_id)
        .bind(2, util::utf8_truncate(title.empty() ? "New chat" : title, kMaxTitleBytes))
        .bind(3, now)
        .bind(4, now);
    s.run();
    return db_.last_insert_rowid();
}

std::vector<ChatSummary> ChatService::list_chats(int64_t user_id) {
    std::vector<ChatSummary> out;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT c.id, c.title, c.created_at, c.updated_at, "
                   "  (SELECT COUNT(*) FROM messages m WHERE m.chat_id=c.id), "
                   "  COALESCE((SELECT m2.content FROM messages m2 WHERE m2.chat_id=c.id "
                   "            ORDER BY m2.id DESC LIMIT 1), '') "
                   "FROM chats c WHERE c.user_id=? AND c.deleted_at IS NULL "
                   "ORDER BY c.updated_at DESC");
        s.bind(1, user_id);
        while (s.step()) {
            ChatSummary c;
            c.id = s.col_int(0);
            c.title = s.col_text(1);
            c.created_at = s.col_int(2);
            c.updated_at = s.col_int(3);
            c.message_count = s.col_int(4);
            c.preview = util::utf8_truncate(s.col_text(5), 90);
            out.push_back(std::move(c));
        }
    } catch (const std::exception&) {
    }
    return out;
}

bool ChatService::chat_belongs_to(int64_t user_id, int64_t chat_id) {
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "SELECT 1 FROM chats WHERE id=? AND user_id=? AND deleted_at IS NULL");
        s.bind(1, chat_id).bind(2, user_id);
        return s.step();
    } catch (const std::exception&) {
        return false;
    }
}

bool ChatService::rename_chat(int64_t user_id, int64_t chat_id, const std::string& title_raw,
                              std::string* error) {
    const std::string title = util::trim(title_raw);
    if (title.empty()) {
        if (error) *error = "A chat name cannot be empty.";
        return false;
    }
    if (title.size() > kMaxTitleBytes) {
        if (error) *error = "That name is too long.";
        return false;
    }
    // Newlines would break the sidebar layout and serve no purpose in a title.
    for (char c : title) {
        if (c == '\n' || c == '\r') {
            if (error) *error = "A chat name must be a single line.";
            return false;
        }
    }

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "UPDATE chats SET title=?, updated_at=? "
                   "WHERE id=? AND user_id=? AND deleted_at IS NULL");
        s.bind(1, title).bind(2, util::now_unix()).bind(3, chat_id).bind(4, user_id);
        s.run();
        if (db_.changes() == 0) {
            if (error) *error = "That chat does not exist.";
            return false;
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    return true;
}

bool ChatService::delete_chat(int64_t user_id, int64_t chat_id) {
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        // Hard delete: the user asked for the chat to be gone, and messages
        // cascade from the foreign key.
        db::Stmt s(db_, "DELETE FROM chats WHERE id=? AND user_id=?");
        s.bind(1, chat_id).bind(2, user_id);
        s.run();
        return db_.changes() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<StoredMessage> ChatService::get_messages(int64_t user_id, int64_t chat_id, int limit) {
    std::vector<StoredMessage> out;
    if (!chat_belongs_to(user_id, chat_id)) return out;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT id, role, content, created_at, off_topic, COALESCE(sources,'') "
                   "FROM messages WHERE chat_id=? ORDER BY id ASC LIMIT ?");
        s.bind(1, chat_id).bind(2, static_cast<int64_t>(limit));
        while (s.step()) {
            StoredMessage m;
            m.id = s.col_int(0);
            m.role = s.col_text(1);
            m.content = s.col_text(2);
            m.created_at = s.col_int(3);
            m.off_topic = s.col_int(4) != 0;
            m.sources = s.col_text(5);
            out.push_back(std::move(m));
        }
    } catch (const std::exception&) {
    }
    return out;
}

int64_t ChatService::insert_message(int64_t chat_id, const std::string& role,
                                    const std::string& content, bool off_topic,
                                    const std::string& sources_json) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    db::Stmt s(db_,
               "INSERT INTO messages(chat_id, role, content, created_at, off_topic, sources) "
               "VALUES(?,?,?,?,?,?)");
    s.bind(1, chat_id).bind(2, role).bind(3, content).bind(4, util::now_unix()).bind(
        5, static_cast<int64_t>(off_topic ? 1 : 0));
    if (sources_json.empty()) s.bind_null(6); else s.bind(6, sources_json);
    s.run();
    return db_.last_insert_rowid();
}

void ChatService::touch_chat(int64_t chat_id) {
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "UPDATE chats SET updated_at=? WHERE id=?");
        s.bind(1, util::now_unix()).bind(2, chat_id);
        s.run();
    } catch (const std::exception&) {
    }
}

std::vector<llm::Message> ChatService::build_prompt(int64_t chat_id, const std::string& question,
                                                    const std::string& context) {
    std::vector<llm::Message> messages;
    messages.push_back({"system", prompts::kTutorSystem});

    // Replay the recent turns so follow-ups resolve. Older turns are dropped
    // rather than summarised: at middle school lengths the last few exchanges
    // carry all the context that matters, and summarising risks distorting the
    // numbers in a problem.
    std::vector<StoredMessage> history;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT id, role, content, created_at, off_topic, '' FROM ("
                   "  SELECT id, role, content, created_at, off_topic FROM messages "
                   "  WHERE chat_id=? ORDER BY id DESC LIMIT ?"
                   ") ORDER BY id ASC");
        s.bind(1, chat_id).bind(2, static_cast<int64_t>(history_turns_ * 2));
        while (s.step()) {
            StoredMessage m;
            m.id = s.col_int(0);
            m.role = s.col_text(1);
            m.content = s.col_text(2);
            history.push_back(std::move(m));
        }
    } catch (const std::exception&) {
    }

    for (const StoredMessage& m : history) {
        if (m.role != "user" && m.role != "assistant") continue;
        messages.push_back({m.role, m.content});
    }

    if (!context.empty()) {
        // Retrieved material goes in as a system turn immediately before the
        // question, so it is clearly instructions-to-the-model rather than
        // something the student said.
        messages.push_back({"system", context});
    }
    messages.push_back({"user", question});
    return messages;
}

AskResult ChatService::ask(const User& user, int64_t chat_id, const std::string& question_raw,
                           const llm::TokenCallback& on_token,
                           const llm::TokenCallback& on_thinking) {
    AskResult result;
    const std::string question = util::trim(question_raw);

    if (question.empty()) {
        result.error = "Type a question first.";
        return result;
    }
    if (question.size() > kMaxQuestionBytes) {
        result.error = "That question is too long. Try breaking it into smaller parts.";
        return result;
    }

    // Resolve the chat, creating one on the first message.
    bool is_new_chat = false;
    if (chat_id == 0) {
        chat_id = create_chat(user.id, derive_title(question));
        is_new_chat = true;
    } else if (!chat_belongs_to(user.id, chat_id)) {
        result.error = "That chat does not exist.";
        return result;
    }
    result.chat_id = chat_id;

    bool has_history = false;
    if (!is_new_chat) {
        try {
            std::lock_guard<std::mutex> lock(db_.mutex());
            db::Stmt s(db_, "SELECT 1 FROM messages WHERE chat_id=? LIMIT 1");
            s.bind(1, chat_id);
            has_history = s.step();
        } catch (const std::exception&) {
        }
    }

    // --- stage 1+2: is this in scope? ------------------------------------
    guard::Decision decision = guard_.classify(question, has_history);

    if (decision.verdict == guard::Verdict::OffTopic ||
        decision.verdict == guard::Verdict::SmallTalk) {
        const bool off_topic = decision.verdict == guard::Verdict::OffTopic;
        const std::string canned =
            off_topic ? prompts::kRefusalMessage : prompts::kSmalltalkMessage;

        result.user_message_id = insert_message(chat_id, "user", question, false, "");
        result.assistant_message_id =
            insert_message(chat_id, "assistant", canned, off_topic, "");
        touch_chat(chat_id);

        // Streaming the canned reply keeps the UI path identical either way.
        if (on_token) on_token(canned);

        result.ok = true;
        result.answer = canned;
        result.off_topic = off_topic;
        if (is_new_chat) {
            result.chat_title = off_topic ? "Off-topic question" : "Hello";
            result.title_changed = true;
            std::string ignored;
            rename_chat(user.id, chat_id, result.chat_title, &ignored);
        }
        return result;
    }

    // --- retrieval --------------------------------------------------------
    std::string context;
    if (retriever_.enabled() && decision.verdict != guard::Verdict::FollowUp) {
        // A bare follow-up ("why?") makes a poor retrieval query; the previous
        // turns already carry the reference material for that thread.
        result.sources = retriever_.retrieve(question, config_.rag_top_k());
        context = rag::Retriever::format_context(result.sources);
    }

    // --- generation -------------------------------------------------------
    std::vector<llm::Message> messages = build_prompt(chat_id, question, context);

    llm::ChatOptions options;
    options.model = client_.default_model();
    options.temperature = config_.ollama_temperature();
    options.num_ctx = config_.ollama_num_ctx();
    options.timeout_s = config_.ollama_timeout_s();
    // Left at the model's default (on, for Qwen3) unless the operator tunes it.
    // Reasoning measurably improves multi-step arithmetic, and Ollama keeps it
    // out of `content`, so the student only ever sees the finished answer.
    options.think = config_.ollama_think();

    // The user turn is stored before generation so a crash mid-answer still
    // leaves a coherent transcript.
    result.user_message_id = insert_message(chat_id, "user", question, false, "");

    llm::ChatResult generated = client_.chat_stream(messages, options, on_token, on_thinking);

    if (!generated.ok) {
        result.error =
            "The tutor model did not respond. Close the app and open it again; if that does "
            "not help, run \"mstutor.exe check\" from a terminal.";
        // Keep the diagnostic out of the user-facing message but do not lose it.
        result.error += "\n\nDetails: " + generated.error;
        touch_chat(chat_id);
        return result;
    }

    std::string answer = util::trim(generated.content);
    if (answer.empty()) {
        result.error = "The tutor model returned an empty answer. Try rephrasing the question.";
        touch_chat(chat_id);
        return result;
    }

    result.assistant_message_id =
        insert_message(chat_id, "assistant", answer, false, sources_to_json(result.sources));
    touch_chat(chat_id);

    result.ok = true;
    result.answer = answer;
    if (is_new_chat) {
        result.chat_title = derive_title(question);
        result.title_changed = true;
    }
    return result;
}

}  // namespace chat
