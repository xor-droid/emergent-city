/* os_client.h — ship records to OpenSearch via a Data Prepper HTTP source, and
 * recall a session back for replay.
 *
 * Writes: POST a JSON array of docs to CSIM_OS_INGEST_URL (Data Prepper /log/ingest,
 *         plain HTTP) which forwards to OpenSearch. Best-effort, never fatal.
 * Reads:  query CSIM_OS_QUERY_URL (OpenSearch _search, basic auth, self-signed TLS)
 *         with CSIM_OS_USER / CSIM_OS_PASS.
 * Without libcurl/cJSON (no HAVE_LLM) these are no-ops / unsupported.
 */
#ifndef OS_CLIENT_H
#define OS_CLIENT_H

int  os_ingest_enabled(void);                 /* CSIM_OS_INGEST_URL is set */
int  os_ingest(const char *json_array);       /* POST docs; 0 ok, <0 error/disabled */

int  os_query_enabled(void);                  /* CSIM_OS_QUERY_URL is set */
/* Fetch all docs for a session from OpenSearch and reconstruct a local session
 * file at out_path (the replay format). 0 ok, <0 error/unsupported. */
int  os_fetch_session(const char *session_id, const char *out_path);

#endif /* OS_CLIENT_H */
