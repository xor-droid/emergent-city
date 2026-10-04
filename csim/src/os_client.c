/* os_client.c — OpenSearch ingest (via Data Prepper) + session recall. See os_client.h. */
#include "os_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAVE_LLM
/* ── no libcurl/cJSON: no-op stubs ───────────────────────────────────────── */
int os_ingest_enabled(void) { return 0; }
int os_ingest(const char *json_array) { (void)json_array; return -1; }
int os_query_enabled(void) { return 0; }
int os_fetch_session(const char *sid, const char *out) { (void)sid; (void)out; return -1; }
#else
#include <curl/curl.h>
#include <cjson/cJSON.h>

typedef struct { char *p; size_t n; } Buf;
static size_t write_cb(void *data, size_t sz, size_t nm, void *ud) {
    size_t add = sz * nm; Buf *b = ud;
    char *np = realloc(b->p, b->n + add + 1); if (!np) return 0;
    b->p = np; memcpy(b->p + b->n, data, add); b->n += add; b->p[b->n] = 0;
    return add;
}

int os_ingest_enabled(void) { const char *u = getenv("CSIM_OS_INGEST_URL"); return u && *u; }
int os_query_enabled(void)  { const char *u = getenv("CSIM_OS_QUERY_URL");  return u && *u; }

/* POST a JSON array of docs to the Data Prepper HTTP source. Best-effort. */
int os_ingest(const char *json_array) {
    const char *url = getenv("CSIM_OS_INGEST_URL");
    if (!url || !*url || !json_array) return -1;
    CURL *c = curl_easy_init(); if (!c) return -2;
    Buf resp = {0};
    struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_array);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 8000L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr); free(resp.p); curl_easy_cleanup(c);
    if (rc != CURLE_OK) return -3;
    return (code >= 200 && code < 300) ? 0 : -4;   /* Data Prepper returns 200 on accept */
}

/* Query OpenSearch for all docs of a session, reconstruct the local session file. */
int os_fetch_session(const char *sid, const char *out_path) {
    const char *base = getenv("CSIM_OS_QUERY_URL");
    if (!base || !*base || !sid || !out_path) return -1;
    char url[1024]; snprintf(url, sizeof url, "%s/csim-*/_search", base);
    char body[512];
    snprintf(body, sizeof body,
        "{\"size\":10000,\"query\":{\"term\":{\"session_id.keyword\":\"%s\"}}}", sid);

    CURL *c = curl_easy_init(); if (!c) return -2;
    Buf resp = {0};
    struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   /* self-signed */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 12000L);
    { const char *u = getenv("CSIM_OS_USER"), *p = getenv("CSIM_OS_PASS");
      if (u) curl_easy_setopt(c, CURLOPT_USERNAME, u);
      if (p) curl_easy_setopt(c, CURLOPT_PASSWORD, p); }
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdr); curl_easy_cleanup(c);
    if (rc != CURLE_OK || !resp.p) { free(resp.p); return -3; }

    cJSON *root = cJSON_Parse(resp.p); free(resp.p);
    if (!root) return -4;
    cJSON *hits = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "hits"), "hits");
    if (!cJSON_IsArray(hits)) { cJSON_Delete(root); return -5; }

    FILE *f = fopen(out_path, "w"); if (!f) { cJSON_Delete(root); return -6; }
    /* two passes: the session manifest first, then events (any order is fine for the parser) */
    cJSON *h;
    cJSON_ArrayForEach(h, hits) {
        cJSON *s = cJSON_GetObjectItem(h, "_source"); if (!s) continue;
        cJSON *dt = cJSON_GetObjectItem(s, "doc_type");
        if (!cJSON_IsString(dt) || strcmp(dt->valuestring, "session")) continue;
        cJSON *it;
        cJSON_ArrayForEach(it, s) {
            const char *k = it->string; if (!k) continue;
            if (!strcmp(k, "session_id"))    fprintf(f, "session %s\n", it->valuestring);
            else if (!strcmp(k, "reason"))   fprintf(f, "reason %s\n", it->valuestring ? it->valuestring : "");
            else if (!strcmp(k, "seed"))     fprintf(f, "seed %.0f\n", it->valuedouble);
            else if (!strcmp(k, "pop"))      fprintf(f, "pop %.0f\n", it->valuedouble);
            else if (!strcmp(k, "llm"))      fprintf(f, "llm %.0f\n", it->valuedouble);
            else if (!strcmp(k, "end_tick")) fprintf(f, "end_tick %.0f\n", it->valuedouble);
            else if (!strcmp(k, "checksum")) fprintf(f, "checksum %s\n", it->valuestring);  /* stored as string */
            else if (!strncmp(k, "cfg_", 4)) fprintf(f, "cfg %s %.10g\n", k + 4, it->valuedouble);
        }
    }
    cJSON_ArrayForEach(h, hits) {
        cJSON *s = cJSON_GetObjectItem(h, "_source"); if (!s) continue;
        cJSON *dt = cJSON_GetObjectItem(s, "doc_type");
        if (!cJSON_IsString(dt) || strcmp(dt->valuestring, "event")) continue;
        double tick = cJSON_GetObjectItem(s, "tick") ? cJSON_GetObjectItem(s, "tick")->valuedouble : 0;
        cJSON *kind = cJSON_GetObjectItem(s, "kind");
        if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "tune")) {
            cJSON *kn = cJSON_GetObjectItem(s, "knob"), *v = cJSON_GetObjectItem(s, "value");
            fprintf(f, "event %.0f tune %s %.10g\n", tick, kn ? kn->valuestring : "?", v ? v->valuedouble : 0);
        } else if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "god")) {
            double tool = cJSON_GetObjectItem(s, "tool") ? cJSON_GetObjectItem(s, "tool")->valuedouble : 0;
            double tx = cJSON_GetObjectItem(s, "tx") ? cJSON_GetObjectItem(s, "tx")->valuedouble : 0;
            double ty = cJSON_GetObjectItem(s, "ty") ? cJSON_GetObjectItem(s, "ty")->valuedouble : 0;
            fprintf(f, "event %.0f god %.0f %.0f %.0f\n", tick, tool, tx, ty);
        }
    }
    fclose(f);
    cJSON_Delete(root);
    return 0;
}
#endif /* HAVE_LLM */
