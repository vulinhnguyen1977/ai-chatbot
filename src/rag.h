// rag.h -- retrieval over the ingested textbook corpus.
//
// The corpus is loaded into doc_chunks by training/ingest_corpus.py. This class
// owns the two things that must happen in-process: turning chunks into vectors
// via Ollama, and finding the passages relevant to a question.
//
// Retrieval is hybrid. A dense pass (cosine similarity over embeddings) catches
// paraphrases -- "how fast is it going" matching a passage about speed -- while
// a lexical pass (SQLite FTS5, BM25 ranked) catches exact terms and symbols
// that embeddings blur. Scores are fused with reciprocal rank fusion, which
// needs no score calibration between the two very different scales.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "db.h"
#include "llm.h"

class Config;

namespace rag {

struct Passage {
    int64_t chunk_id = 0;
    std::string source;
    std::string subject;
    std::string grade;
    std::string locator;
    std::string content;
    double score = 0.0;
};

struct IndexStats {
    int64_t chunks = 0;
    int64_t embedded = 0;
    std::string embed_model;
};

class Retriever {
public:
    Retriever(db::Database& database, llm::Client& client, const Config& config);

    IndexStats stats();

    // Embeds every chunk that has no vector for the current embedding model.
    // `progress` is called as (done, total). Returns the number embedded.
    int64_t build_embeddings(const std::function<void(int64_t, int64_t)>& progress,
                             std::string* error);

    // Returns up to `top_k` passages relevant to `query`, best first. Never
    // throws: on any failure it returns an empty list and the caller answers
    // without reference material.
    std::vector<Passage> retrieve(const std::string& query, int top_k);

    // Formats passages for injection into the tutor prompt.
    static std::string format_context(const std::vector<Passage>& passages);

    // Drops the cached vectors so the next retrieve() reloads from disk.
    void invalidate_cache();

    bool enabled() const { return enabled_; }

private:
    struct CachedVector {
        int64_t chunk_id;
        std::vector<float> vec;  // stored L2-normalised
    };

    void ensure_cache_loaded();
    std::vector<Passage> dense_search(const std::string& query, int limit);
    std::vector<Passage> lexical_search(const std::string& query, int limit);
    bool load_passage(int64_t chunk_id, Passage* out);

    db::Database& db_;
    llm::Client& client_;
    bool enabled_ = true;
    int top_k_ = 4;
    double min_score_ = 0.35;
    std::string embed_model_;

    std::mutex cache_mutex_;
    bool cache_loaded_ = false;
    std::vector<CachedVector> cache_;
};

}  // namespace rag
