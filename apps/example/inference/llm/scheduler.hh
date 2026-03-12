#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <deque>

#include "pump/core/lock_free_queue.hh"
#include "llama.h"

namespace inference {

// Request submitted to the LLM scheduler
struct generate_req {
    std::string prompt;
    // Called for each generated token. text/len is the decoded UTF-8 piece.
    // is_eos=true means generation finished (text may be empty).
    std::move_only_function<void(const char* text, uint32_t len, bool is_eos)> on_token;
    uint32_t max_tokens = 512;
};

// LLM scheduler: wraps llama.cpp, one decode step per advance() call.
//
// Phase 1: synchronous llama_decode (blocks ~10-30ms per token).
// Phase 2: async via ggml graph_compute_async + cuEventQuery polling.
struct llm_scheduler {
    // --- llama resources ---
    llama_model*   model   = nullptr;
    llama_context* ctx     = nullptr;
    llama_sampler* sampler = nullptr;
    const llama_vocab* vocab = nullptr;

    // --- request queue (cross-core safe) ---
    pump::core::per_core::queue<generate_req*> req_q;

    // --- in-flight generation state ---
    struct inflight {
        generate_req* req;
        int n_decoded = 0;
    };
    inflight*                  current = nullptr;
    llama_token                next_token = 0;
    std::deque<generate_req*>  pending;

    // ---------------------------------------------------------------
    // Init: load model + create context + init sampler
    // ---------------------------------------------------------------
    bool init(const char* model_path, int n_ctx = 4096, int n_gpu_layers = 999) {
        // Load model
        auto mparams = llama_model_default_params();
        mparams.n_gpu_layers = n_gpu_layers;

        model = llama_model_load_from_file(model_path, mparams);
        if (!model) {
            fprintf(stderr, "[llm] failed to load model: %s\n", model_path);
            return false;
        }

        vocab = llama_model_get_vocab(model);

        // Create context
        auto cparams = llama_context_default_params();
        cparams.n_ctx   = n_ctx;
        cparams.n_batch = n_ctx;

        ctx = llama_init_from_model(model, cparams);
        if (!ctx) {
            fprintf(stderr, "[llm] failed to create context\n");
            return false;
        }

        // Sampler chain: temp → top_p → dist
        sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.8f));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(42));

        fprintf(stderr, "[llm] model loaded, n_ctx=%d, gpu_layers=%d\n", n_ctx, n_gpu_layers);
        return true;
    }

    // Enqueue a generate request (cross-core safe via per_core::queue)
    void schedule(generate_req* r) {
        req_q.try_enqueue(r);
    }

    // ---------------------------------------------------------------
    // advance(): called in the main polling loop.
    // Does at most ONE decode step (~10-30ms) then returns.
    // ---------------------------------------------------------------
    bool advance() {
        // 1. Continue current generation
        if (current) {
            decode_one_step();
            return true;
        }

        // 2. Pick up new requests
        bool got = false;
        req_q.drain([this, &got](generate_req* r) {
            got = true;
            if (!current) {
                start_generation(r);
            } else {
                pending.push_back(r);
            }
        });
        return got;
    }

    ~llm_scheduler() {
        if (sampler) llama_sampler_free(sampler);
        if (ctx)     llama_free(ctx);
        if (model)   llama_model_free(model);
    }

private:
    // ---------------------------------------------------------------
    // Start a new generation: tokenize prompt → prefill → sample first token
    // ---------------------------------------------------------------
    void start_generation(generate_req* r) {
        // Clear KV cache for fresh generation
        llama_memory_clear(llama_get_memory(ctx), true);
        llama_sampler_reset(sampler);

        // Tokenize
        int n_max = r->prompt.size() + 32;
        std::vector<llama_token> tokens(n_max);
        int n = llama_tokenize(
            vocab,
            r->prompt.c_str(), r->prompt.size(),
            tokens.data(), n_max,
            true,   // add_special (BOS)
            false   // parse_special
        );

        if (n < 0) {
            r->on_token("tokenize failed", 15, true);
            delete r;
            return;
        }
        tokens.resize(n);

        fprintf(stderr, "[llm] prefill %d tokens\n", n);

        // Prefill (blocking — all prompt tokens at once)
        llama_batch batch = llama_batch_get_one(tokens.data(), n);
        if (llama_decode(ctx, batch) != 0) {
            r->on_token("prefill failed", 14, true);
            delete r;
            return;
        }

        // Sample first token
        llama_token token = llama_sampler_sample(sampler, ctx, -1);
        llama_sampler_accept(sampler, token);

        current = new inflight{r, 0};

        if (llama_vocab_is_eog(vocab, token)) {
            r->on_token("", 0, true);
            finish_current();
            return;
        }

        emit_token(token);
        current->n_decoded = 1;
        next_token = token;
    }

    // ---------------------------------------------------------------
    // One decode step: decode the pending token → sample → emit
    // ---------------------------------------------------------------
    void decode_one_step() {
        llama_batch batch = llama_batch_get_one(&next_token, 1);

        if (llama_decode(ctx, batch) != 0) {
            current->req->on_token("decode failed", 13, true);
            finish_current();
            return;
        }

        llama_token token = llama_sampler_sample(sampler, ctx, -1);
        llama_sampler_accept(sampler, token);
        current->n_decoded++;

        if (llama_vocab_is_eog(vocab, token) ||
            current->n_decoded >= (int)current->req->max_tokens) {
            current->req->on_token("", 0, true);
            finish_current();
            return;
        }

        emit_token(token);
        next_token = token;
    }

    // ---------------------------------------------------------------
    // Convert token to text and call the callback
    // ---------------------------------------------------------------
    void emit_token(llama_token token) {
        char buf[256];
        int len = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, true);
        if (len > 0) {
            current->req->on_token(buf, len, false);
        }
    }

    // ---------------------------------------------------------------
    // Finish current request, start next pending if any
    // ---------------------------------------------------------------
    void finish_current() {
        delete current->req;
        delete current;
        current = nullptr;

        if (!pending.empty()) {
            auto* next = pending.front();
            pending.pop_front();
            start_generation(next);
        }
    }
};

} // namespace inference
