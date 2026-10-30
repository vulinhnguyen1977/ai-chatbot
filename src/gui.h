// gui.h -- the native desktop application.
//
// There is no browser and no HTTP server: the window talks straight to the
// service objects in-process. Nothing listens on a socket except the bundled
// model server, which binds loopback only.
#pragma once

#include <string>

#include "auth.h"
#include "chat.h"
#include "config.h"
#include "db.h"
#include "guard.h"
#include "llm.h"
#include "mailer.h"
#include "rag.h"
#include "runtime.h"

namespace gui {

// Everything the window needs. Owned by main(), outlives the window.
struct AppContext {
    Config& config;
    db::Database& database;
    Auth& auth;
    chat::ChatService& chats;
    llm::Client& client;
    rag::Retriever& retriever;
    Mailer& mailer;
    runtime::LocalRuntime& runtime;
};

// Creates the window and runs the message loop. Returns the process exit code.
int run(AppContext& context);

}  // namespace gui
