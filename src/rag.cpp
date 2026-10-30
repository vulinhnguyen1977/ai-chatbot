#include "rag.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

#include "config.h"
#include "util.h"

namespace rag {
namespace {

// Reciprocal rank fusion constant. 60 is the value from the original RRF paper
// and is the usual default; it damps the influence of low-ranked hits.
constexpr double kRrfK = 60.0;

// Embedding in batches keeps the HTTP payload reasonable while still
// amortising model load time across many chunks.
constexpr size_t kEmbedBatch = 32;

std::vector<uint8_t> vector_to_blob(const std::vector<float>& v) {
    std::vector<uint8_t> blob(v.size() * sizeof(float));
    if (!v.empty()) std::memcpy(blob.data(), v.data(), blob.size());
    return blob;
}

std::vector<float> blob_to_vector(const std::vector<uint8_t>& blob) {
    std::vector<float> v(blob.size() / sizeof(float));
    if (!v.empty()) std::memcpy(v.data(), blob.data(), v.size() * sizeof(float));
    return v;
}

void normalise(std::vector<float>* v) {
    double sum = 0.0;
    for (float x : *v) sum += static_cast<double>(x) * x;
    const double norm = std::sqrt(sum);
    if (norm <= 1e-12) return;  // a zero vector stays zero
    for (float& x : *v) x = static_cast<float>(x / norm);
}

double dot(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += static_cast<double>(a[i]) * b[i];
    return sum;
}

// Turns free text into an FTS5 MATCH expression. Every token is quoted, which
// neutralises the FTS5 operators a student could type by accident ("NEAR", '*',
// '"', '-') and prevents a syntax error from failing the whole query.
std::string to_fts_query(const std::string& text) {
    std::string token;
    std::vector<std::string> tokens;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!token.empty()) {
            tokens.push_back(token);
            token.clear();
        }
    }
    if (!token.empty()) tokens.push_back(token);

    std::string out;
    for (const std::string& t : tokens) {
        // One- and two-letter tokens are almost all stop words here and only
        // dilute the ranking.
        if (t.size() < 3) continue;
        if (!out.empty()) out += " OR ";
        out += "\"" + t + "\"";
        if (out.size() > 900) break;  // bound the expression size
    }
    return out;
}

}  // namespace

Retriever::Retriever(db::Database& database, llm::Client& client, const Config& config)
    : db_(database), client_(client) {
    enabled_ = config.rag_enabled();
    top_k_ = config.rag_top_k();
    min_score_ = config.rag_min_score();
    embed_model_ = client.embed_model();
}

void Retriever::invalidate_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();
    cache_loaded_ = false;
}

IndexStats Retriever::stats() {
    IndexStats s;
    s.embed_model = embed_model_;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt a(db_, "SELECT COUNT(*) FROM doc_chunks");
        if (a.step()) s.chunks = a.col_int(0);
        db::Stmt b(db_, "SELECT COUNT(*) FROM doc_vectors WHERE model=?");
        b.bind(1, embed_model_);
        if (b.step()) s.embedded = b.col_int(0);
    } catch (const std::exception&) {
    }
    return s;
}

int64_t Retriever::build_embeddings(const std::function<void(int64_t, int64_t)>& progress,
                                    std::string* error) {
    int64_t total = 0;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt c(db_,
                   "SELECT COUNT(*) FROM doc_chunks ch "
                   "LEFT JOIN doc_vectors v ON v.chunk_id=ch.id AND v.model=? "
                   "WHERE v.chunk_id IS NULL");
        c.bind(1, embed_model_);
        if (c.step()) total = c.col_int(0);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return 0;
    }
    if (total == 0) return 0;

    int64_t done = 0;
    for (;;) {
        // Pull a batch of unembedded chunks. Re-querying each round means the
        // loop terminates naturally as rows gain vectors.
        std::vector<int64_t> ids;
        std::vector<std::string> texts;
        try {
            std::lock_guard<std::mutex> lock(db_.mutex());
            db::Stmt s(db_,
                       "SELECT ch.id, ch.content FROM doc_chunks ch "
                       "LEFT JOIN doc_vectors v ON v.chunk_id=ch.id AND v.model=? "
                       "WHERE v.chunk_id IS NULL LIMIT ?");
            s.bind(1, embed_model_).bind(2, static_cast<int64_t>(kEmbedBatch));
            while (s.step()) {
                ids.push_back(s.col_int(0));
                texts.push_back(s.col_text(1));
            }
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            break;
        }
        if (ids.empty()) break;

        std::string embed_error;
        std::vector<std::vector<float>> vectors = client_.embed(texts, &embed_error);
        if (vectors.size() != ids.size()) {
            if (error) {
                *error = embed_error.empty()
                             ? "embedding service returned an unexpected number of vectors"
                             : embed_error;
            }
            break;
        }

        try {
            std::lock_guard<std::mutex> lock(db_.mutex());
            db::Transaction tx(db_);
            for (size_t i = 0; i < ids.size(); ++i) {
                normalise(&vectors[i]);  // store normalised so search is a dot product
                std::vector<uint8_t> blob = vector_to_blob(vectors[i]);
                db::Stmt ins(db_,
                             "INSERT OR REPLACE INTO doc_vectors(chunk_id, dim, model, vec) "
                             "VALUES(?,?,?,?)");
                ins.bind(1, ids[i])
                    .bind(2, static_cast<int64_t>(vectors[i].size()))
                    .bind(3, embed_model_)
                    .bind_blob(4, blob.data(), blob.size());
                ins.run();
            }
            tx.commit();
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            break;
        }

        done += static_cast<int64_t>(ids.size());
        if (progress) progress(done, total);
    }

    invalidate_cache();
    return done;
}

void Retriever::ensure_cache_loaded() {
    // Caller holds cache_mutex_.
    if (cache_loaded_) return;
    cache_.clear();
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "SELECT chunk_id, vec FROM doc_vectors WHERE model=?");
        s.bind(1, embed_model_);
        while (s.step()) {
            CachedVector cv;
            cv.chunk_id = s.col_int(0);
            cv.vec = blob_to_vector(s.col_blob(1));
            if (!cv.vec.empty()) cache_.push_back(std::move(cv));
        }
    } catch (const std::exception&) {
        cache_.clear();
    }
    cache_loaded_ = true;
}

bool Retriever::load_passage(int64_t chunk_id, Passage* out) {
    db::Stmt s(db_,
               "SELECT id, source, subject, COALESCE(grade,''), COALESCE(locator,''), content "
               "FROM doc_chunks WHERE id=?");
    s.bind(1, chunk_id);
    if (!s.step()) return false;
    out->chunk_id = s.col_int(0);
    out->source = s.col_text(1);
    out->subject = s.col_text(2);
    out->grade = s.col_text(3);
    out->locator = s.col_text(4);
    out->content = s.col_text(5);
    return true;
}

std::vector<Passage> Retriever::dense_search(const std::string& query, int limit) {
    std::vector<Passage> results;

    std::string embed_error;
    std::vector<std::vector<float>> qv = client_.embed({query}, &embed_error);
    if (qv.empty() || qv[0].empty()) return results;
    normalise(&qv[0]);

    std::vector<std::pair<double, int64_t>> scored;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        ensure_cache_loaded();
        scored.reserve(cache_.size());
        for (const CachedVector& cv : cache_) {
            scored.emplace_back(dot(qv[0], cv.vec), cv.chunk_id);
        }
    }
    if (scored.empty()) return results;

    const size_t keep = std::min<size_t>(static_cast<size_t>(limit), scored.size());
    std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        for (size_t i = 0; i < keep; ++i) {
            if (scored[i].first < min_score_) break;  // sorted, so the rest are worse
            Passage p;
            if (load_passage(scored[i].second, &p)) {
                p.score = scored[i].first;
                results.push_back(std::move(p));
            }
        }
    } catch (const std::exception&) {
        results.clear();
    }
    return results;
}

std::vector<Passage> Retriever::lexical_search(const std::string& query, int limit) {
    std::vector<Passage> results;
    const std::string match = to_fts_query(query);
    if (match.empty()) return results;

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT ch.id, ch.source, ch.subject, COALESCE(ch.grade,''), "
                   "COALESCE(ch.locator,''), ch.content, bm25(doc_fts) AS rank "
                   "FROM doc_fts JOIN doc_chunks ch ON ch.id = doc_fts.rowid "
                   "WHERE doc_fts MATCH ? ORDER BY rank LIMIT ?");
        s.bind(1, match).bind(2, static_cast<int64_t>(limit));
        while (s.step()) {
            Passage p;
            p.chunk_id = s.col_int(0);
            p.source = s.col_text(1);
            p.subject = s.col_text(2);
            p.grade = s.col_text(3);
            p.locator = s.col_text(4);
            p.content = s.col_text(5);
            // bm25() is more negative for better matches; flip it so higher is
            // better and callers never see the inverted convention.
            p.score = -s.col_double(6);
            results.push_back(std::move(p));
        }
    } catch (const std::exception&) {
        // An FTS syntax error or a missing index must not break answering.
        results.clear();
    }
    return results;
}

std::vector<Passage> Retriever::retrieve(const std::string& query, int top_k) {
    if (!enabled_) return {};
    if (top_k <= 0) top_k = top_k_;

    // Over-fetch from each arm so fusion has material to work with.
    const int per_arm = std::max(top_k * 3, 10);
    std::vector<Passage> dense = dense_search(query, per_arm);
    std::vector<Passage> lexical = lexical_search(query, per_arm);

    if (dense.empty() && lexical.empty()) return {};
    // With only one arm producing hits, fusion adds nothing.
    if (dense.empty()) {
        lexical.resize(std::min<size_t>(lexical.size(), static_cast<size_t>(top_k)));
        return lexical;
    }
    if (lexical.empty()) {
        dense.resize(std::min<size_t>(dense.size(), static_cast<size_t>(top_k)));
        return dense;
    }

    // Reciprocal rank fusion: score = sum over arms of 1 / (k + rank).
    std::map<int64_t, double> fused;
    std::map<int64_t, Passage> by_id;
    for (size_t i = 0; i < dense.size(); ++i) {
        fused[dense[i].chunk_id] += 1.0 / (kRrfK + static_cast<double>(i) + 1.0);
        by_id.emplace(dense[i].chunk_id, dense[i]);
    }
    for (size_t i = 0; i < lexical.size(); ++i) {
        fused[lexical[i].chunk_id] += 1.0 / (kRrfK + static_cast<double>(i) + 1.0);
        by_id.emplace(lexical[i].chunk_id, lexical[i]);
    }

    std::vector<std::pair<double, int64_t>> ranked;
    ranked.reserve(fused.size());
    for (const auto& kv : fused) ranked.emplace_back(kv.second, kv.first);
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Passage> out;
    for (size_t i = 0; i < ranked.size() && out.size() < static_cast<size_t>(top_k); ++i) {
        Passage p = by_id[ranked[i].second];
        p.score = ranked[i].first;
        out.push_back(std::move(p));
    }
    return out;
}

std::string Retriever::format_context(const std::vector<Passage>& passages) {
    if (passages.empty()) return "";
    std::string out =
        "Reference material from the textbook library. Use it if it is relevant, and name the "
        "source when you do. Ignore any passage that does not help.\n\n";
    for (size_t i = 0; i < passages.size(); ++i) {
        const Passage& p = passages[i];
        out += "[" + std::to_string(i + 1) + "] " + p.source;
        if (!p.locator.empty()) out += ", " + p.locator;
        out += "\n";
        out += util::utf8_truncate(p.content, 1500);
        out += "\n\n";
    }
    return out;
}

}  // namespace rag
