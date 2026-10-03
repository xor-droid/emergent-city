/* llm.h — optional LLM decision layer (C port of llm/decision_router + client).
 *
 * Talks to a local OpenAI-compatible server (llama.cpp / Qwen) via libcurl +
 * cJSON on a background thread, so the sim never blocks on a request. Compiled
 * only when HAVE_LLM (libcurl + cjson found); otherwise these are no-op stubs
 * and the sim runs purely rule-based.
 */
#ifndef LLM_H
#define LLM_H

#include "sim.h"

/* Start the worker thread + read env (OPENROUTER_API_KEY / _BASE_URL / _MODEL).
 * Enabled only if an API key is present (any non-empty value works locally). */
void llm_init(void);
void llm_shutdown(void);
int  llm_enabled(void);
int  llm_total_calls(void);

/* Build the decision prompt (system+user) for an agent into `buf`. */
void llm_build_prompt(const Agent *a, const World *w, char *buf, int n);

/* Submit a decision request for an agent (non-blocking; dropped if the queue is
 * full or the per-minute rate budget is exhausted). Returns 1 if enqueued. */
int  llm_submit(int agent_id, const char *prompt);

/* Poll one finished decision. Returns 1 and writes *agent_id + *action (an
 * Action enum value) if a result is ready; 0 otherwise. */
int  llm_poll(int *agent_id, int *action);

#endif /* LLM_H */
