/* llm.c — background LLM decision client (libcurl + cJSON), or no-op stubs.
 *
 * Mirrors the Python llm/ layer: an OpenAI-compatible chat/completions client
 * pointed at the local Qwen server, a worker thread so the sim never blocks, and
 * the decision prompt (ranked needs + personality + time of day, /no_think). */
#include "llm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Rate / gating (mirrors config.py). */
#define LLM_RATE_PER_MIN 200

#ifndef HAVE_LLM
/* ── Stubs: sim runs purely rule-based ───────────────────────────────────── */
void llm_init(void) {}
void llm_shutdown(void) {}
int  llm_enabled(void) { return 0; }
int  llm_total_calls(void) { return 0; }
int  llm_submit(int agent_id, const char *prompt) { (void)agent_id;(void)prompt; return 0; }
int  llm_poll(int *agent_id, int *action) { (void)agent_id;(void)action; return 0; }

#else
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>

#define REQ_CAP 16
#define RES_CAP 64
#define PROMPT_MAX 768

static const char *SYSTEM_DECISION =
    "You are the instinct of a city resident, choosing their next action. "
    "A need only counts as URGENT when it is low (below ~0.40); a need near 0 "
    "overrides everything. If every need is above 0.40 the person is comfortable, "
    "so do NOT pick by tiny need differences - decide by time of day and their "
    "personality/goals. Need->action guide: eat/shop=hunger, sleep/go_home=energy, "
    "socialize/drink_at_bar=social, work=money+meaning, pray=meaning+belonging, "
    "commit_crime=money (risky), flee=danger, wander=restless. Never default to "
    "'eat' unless hunger is actually low. Reply with EXACTLY ONE word from this "
    "list and nothing else: eat, sleep, work, socialize, drink_at_bar, go_home, "
    "wander, pray, shop, commit_crime, flee. /no_think";

typedef struct { int agent_id; char user[PROMPT_MAX]; } Req;
typedef struct { int agent_id; int action; } Res;

static struct {
    Req req[REQ_CAP]; int rhead, rtail, rcount;
    Res res[RES_CAP]; int shead, stail, scount;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    pthread_t worker;
    int running, enabled, total_calls;
    int calls_this_min; time_t window;
    char base_url[256], model[128], api_key[256];
} L;

/* ── word -> Action ──────────────────────────────────────────────────────── */
static int word_to_action(const char *w) {
    if (!strcmp(w, "eat")) return A_EAT;
    if (!strcmp(w, "sleep")) return A_SLEEP;
    if (!strcmp(w, "work")) return A_WORK;
    if (!strcmp(w, "socialize")) return A_SOCIALIZE;
    if (!strcmp(w, "drink_at_bar") || !strcmp(w, "drink")) return A_DRINK;
    if (!strcmp(w, "go_home")) return A_GO_HOME;
    if (!strcmp(w, "wander")) return A_WANDER;
    if (!strcmp(w, "pray")) return A_PRAY;
    if (!strcmp(w, "shop")) return A_SHOP;
    if (!strcmp(w, "commit_crime") || !strcmp(w, "crime")) return A_CRIME;
    if (!strcmp(w, "flee")) return A_FLEE;
    return -1;
}

/* ── HTTP response accumulator ───────────────────────────────────────────── */
typedef struct { char *data; size_t len; } Buf;
static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm;
    Buf *b = (Buf *)ud;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Do one blocking request; return Action or -1. Runs on the worker thread. */
static int do_request(CURL *curl, const char *user_prompt) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", L.model);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", SYSTEM_DECISION);
    cJSON_AddItemToArray(msgs, sys);
    cJSON *usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", user_prompt);
    cJSON_AddItemToArray(msgs, usr);
    cJSON_AddNumberToObject(root, "max_tokens", 8);
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    Buf buf = {0};
    char auth[300];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", L.api_key);
    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, auth);

    curl_easy_setopt(curl, CURLOPT_URL, L.base_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 12000L);

    int action = -1;
    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK && buf.data) {
        cJSON *resp = cJSON_Parse(buf.data);
        if (resp) {
            cJSON *choices = cJSON_GetObjectItem(resp, "choices");
            cJSON *c0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
            cJSON *msg = c0 ? cJSON_GetObjectItem(c0, "message") : NULL;
            const char *txt = NULL;
            if (msg) {
                cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (content && cJSON_IsString(content) && content->valuestring[0])
                    txt = content->valuestring;
                else {
                    cJSON *rc2 = cJSON_GetObjectItem(msg, "reasoning_content");
                    if (rc2 && cJSON_IsString(rc2)) txt = rc2->valuestring;
                }
            }
            if (txt) {
                char word[32]; int wi = 0;
                for (const char *p = txt; *p && wi < 31; p++) {
                    if (isspace((unsigned char)*p) || *p == '.' || *p == ',' || *p == '!' ) {
                        if (wi) break; else continue;
                    }
                    word[wi++] = (char)tolower((unsigned char)*p);
                }
                word[wi] = '\0';
                action = word_to_action(word);
            }
            cJSON_Delete(resp);
        }
    }
    free(buf.data);
    free(body);
    curl_slist_free_all(hdr);
    return action;
}

static void *worker_main(void *arg) {
    (void)arg;
    CURL *curl = curl_easy_init();
    for (;;) {
        pthread_mutex_lock(&L.mtx);
        while (L.rcount == 0 && L.running) pthread_cond_wait(&L.cv, &L.mtx);
        if (!L.running && L.rcount == 0) { pthread_mutex_unlock(&L.mtx); break; }
        Req r = L.req[L.rhead];
        L.rhead = (L.rhead + 1) % REQ_CAP; L.rcount--;
        pthread_mutex_unlock(&L.mtx);

        int action = curl ? do_request(curl, r.user) : -1;

        pthread_mutex_lock(&L.mtx);
        L.total_calls++;
        if (action >= 0 && L.scount < RES_CAP) {
            L.res[L.stail].agent_id = r.agent_id;
            L.res[L.stail].action = action;
            L.stail = (L.stail + 1) % RES_CAP; L.scount++;
        }
        pthread_mutex_unlock(&L.mtx);
    }
    if (curl) curl_easy_cleanup(curl);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void llm_init(void) {
    memset(&L, 0, sizeof(L));
    const char *key = getenv("OPENROUTER_API_KEY");
    const char *url = getenv("OPENROUTER_BASE_URL");
    const char *model = getenv("OPENROUTER_MODEL");
    snprintf(L.api_key, sizeof(L.api_key), "%s", key ? key : "");
    snprintf(L.base_url, sizeof(L.base_url), "%s",
             (url && url[0]) ? url : "http://10.0.0.134:9090/v1/chat/completions");
    snprintf(L.model, sizeof(L.model), "%s",
             (model && model[0]) ? model : "Qwen/Qwen3-8B-GGUF:Q4_K_M");
    L.enabled = (L.api_key[0] != '\0');
    if (!L.enabled) return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_mutex_init(&L.mtx, NULL);
    pthread_cond_init(&L.cv, NULL);
    L.running = 1;
    L.window = time(NULL);
    pthread_create(&L.worker, NULL, worker_main, NULL);
}

void llm_shutdown(void) {
    if (!L.enabled) return;
    pthread_mutex_lock(&L.mtx);
    L.running = 0;
    pthread_cond_signal(&L.cv);
    pthread_mutex_unlock(&L.mtx);
    pthread_join(L.worker, NULL);
    curl_global_cleanup();
}

int llm_enabled(void) { return L.enabled; }
int llm_total_calls(void) { return L.total_calls; }

int llm_submit(int agent_id, const char *prompt) {
    if (!L.enabled) return 0;
    int ok = 0;
    pthread_mutex_lock(&L.mtx);
    time_t now = time(NULL);
    if (now - L.window >= 60) { L.window = now; L.calls_this_min = 0; }
    if (L.calls_this_min < LLM_RATE_PER_MIN && L.rcount < REQ_CAP) {
        L.calls_this_min++;
        L.req[L.rtail].agent_id = agent_id;
        snprintf(L.req[L.rtail].user, PROMPT_MAX, "%s", prompt);
        L.rtail = (L.rtail + 1) % REQ_CAP; L.rcount++;
        pthread_cond_signal(&L.cv);
        ok = 1;
    }
    pthread_mutex_unlock(&L.mtx);
    return ok;
}

int llm_poll(int *agent_id, int *action) {
    if (!L.enabled) return 0;
    int got = 0;
    pthread_mutex_lock(&L.mtx);
    if (L.scount > 0) {
        *agent_id = L.res[L.shead].agent_id;
        *action = L.res[L.shead].action;
        L.shead = (L.shead + 1) % RES_CAP; L.scount--;
        got = 1;
    }
    pthread_mutex_unlock(&L.mtx);
    return got;
}
#endif /* HAVE_LLM */

/* ── Prompt builder (compiled in both modes; pure string work) ───────────── */
static const char *TRAIT_NAMES[TRAIT_COUNT] = {
    "cruel","manipulative","vengeful","kind","religious","awkward","charismatic",
    "lazy","ambitious","disciplined","skeptical","cynical","alcoholic","brave"};

void llm_build_prompt(const Agent *a, const World *w, char *buf, int n) {
    char traits[160]; traits[0] = '\0';
    for (int i = 0; i < TRAIT_COUNT; i++)
        if (a->pers.traits & (1u << i)) {
            if (traits[0]) strncat(traits, ", ", sizeof(traits) - strlen(traits) - 1);
            strncat(traits, TRAIT_NAMES[i], sizeof(traits) - strlen(traits) - 1);
        }
    if (!traits[0]) snprintf(traits, sizeof(traits), "-");

    const Needs *nd = &a->needs;
    char low[160]; low[0] = '\0';
    const char *lbl[6] = {"hunger","energy","safety","social","meaning","belonging"};
    double val[6] = {nd->hunger, nd->energy, nd->safety, nd->social, nd->meaning, nd->belonging};
    for (int i = 0; i < 6; i++) if (val[i] < NEED_LOW) {
        char seg[32]; snprintf(seg, sizeof(seg), "%s%s=%.2f", low[0]?", ":"", lbl[i], val[i]);
        strncat(low, seg, sizeof(low) - strlen(low) - 1);
    }
    int hour = (int)w->hour;
    int night = world_is_night(w);

    char summary[220];
    if (low[0]) snprintf(summary, sizeof(summary), "URGENT low needs: %s", low);
    else        snprintf(summary, sizeof(summary), "No urgent needs - comfortable.");

    snprintf(buf, (size_t)n,
        "%s, age %d. Traits: %s.\n"
        "Personality: O=%.2f C=%.2f E=%.2f A=%.2f N=%.2f.\n"
        "Needs: hunger=%.2f energy=%.2f safety=%.2f social=%.2f meaning=%.2f belonging=%.2f money=%.0f.\n"
        "%s\n"
        "Time: %02d:00, %s.\n"
        "Their one best action right now (one word):",
        a->name, a->age, traits,
        a->pers.o, a->pers.c, a->pers.e, a->pers.a, a->pers.n,
        nd->hunger, nd->energy, nd->safety, nd->social, nd->meaning, nd->belonging, nd->money,
        summary, hour, night ? "night (most people sleep)" : "daytime");
}
