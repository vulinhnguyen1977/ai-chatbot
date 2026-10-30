// chat.h -- conversations, their history, and the ask() pipeline.
//
// Every read and write is scoped by user_id, so a chat id belonging to another
// account simply does not resolve. That check lives here rather than in the
// HTTP layer so no future route can forget it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "auth.h"
#include "db.h"
#include "guard.h"
#include "llm.h"
#include "rag.h"

class Config;

namespace chat {

struct ChatSummary {
    int64_t id = 0;
    std::string title;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t message_count = 0;
    std::string preview;
};

struct StoredMessage {
    int64_t id = 0;
    std::string role;
    std::string content;
    int64_t created_at = 0;
    bool off_topic = false;
    std::string sources;  // JSON array, empty when nothing was retrieved
};

struct AskResult {
    bool ok = false;
    std::string error;
    std::string answer;
    int64_t chat_id = 0;
    int64_t user_message_id = 0;
    int64_t assistant_message_id = 0;
    bool off_topic = false;
    std::string chat_title;
    bool title_changed = false;
    std::vector<rag::Passage> sources;
};

class ChatService {
public:
    ChatService(db::Database& database, llm::Client& client, guard::DomainGuard& domain_guard,
                rag::Retriever& retriever, const Config& config);

    int64_t create_chat(int64_t user_id, const std::string& title);
    std::vector<ChatSummary> list_chats(int64_t user_id);
    bool rename_chat(int64_t user_id, int64_t chat_id, const std::string& title,
                     std::string* error);
    bool delete_chat(int64_t user_id, int64_t chat_id);
    bool chat_belongs_to(int64_t user_id, int64_t chat_id);
    std::vector<StoredMessage> get_messages(int64_t user_id, int64_t chat_id, int limit = 500);

    // Runs the full pipeline: topic gate, retrieval, generation, persistence.
    // `chat_id` of 0 creates a new chat. Either callback may be null.
    // `on_thinking` is a liveness signal only -- reasoning is never stored and
    // never shown as part of the answer.
    AskResult ask(const User& user, int64_t chat_id, const std::string& question,
                  const llm::TokenCallback& on_token,
                  const llm::TokenCallback& on_thinking = nullptr);

private:
    std::vector<llm::Message> build_prompt(int64_t chat_id, const std::string& question,
                                           const std::string& context);
    // Derives a chat title from the first question, without a model call.
    static std::string derive_title(const std::string& question);
    int64_t insert_message(int64_t chat_id, const std::string& role, const std::string& content,
                           bool off_topic, const std::string& sources_json);
    void touch_chat(int64_t chat_id);

    db::Database& db_;
    llm::Client& client_;
    guard::DomainGuard& guard_;
    rag::Retriever& retriever_;
    const Config& config_;
    int history_turns_ = 8;
};

}  // namespace chat
