/* record.h — session recording + deterministic replay.
 *
 * A session is reproducible from: seed + startup config + the tick-stamped timeline
 * of interventions (live tuning changes and god-mode actions). Replay re-runs from the
 * seed, re-applying each event when the step clock (World.tick) reaches its stamp, and
 * verifies a final-state checksum — proving the replay is byte-identical.
 *
 * The local session file is a simple line format (no JSON dependency); the OpenSearch
 * sink (os_client) serialises the same data to JSON for ingestion.
 */
#ifndef RECORD_H
#define RECORD_H
#include <stdint.h>
#include "sim.h"

/* FNV-1a over the whole World — the byte-identical fingerprint. */
uint64_t world_checksum(const World *w);

/* ── recording (writer) ──────────────────────────────────────────────────── */
void rec_begin(uint64_t seed, int pop, const char *reason);  /* snapshot startup config */
int  rec_active(void);
void rec_tune(const World *w, const char *knob, double value); /* a live knob change */
void rec_tune_now(const World *w, const char *knob);           /* record a knob's current value */
void rec_god(const World *w, int tool, int tx, int ty);        /* a god-mode action */
void rec_end(const World *w);                                  /* stamp end tick + checksum + ship to OpenSearch */
int  rec_save_file(const char *path);                          /* write the session; 1 ok */
const char *rec_session_id(void);                              /* active session id, or "" */

/* ── replay (reader) ─────────────────────────────────────────────────────── */
/* Re-run a session file deterministically, applying its events, and check the
 * final checksum. Returns 0 = ok & checksum matched, 1 = ran but checksum MISMATCH,
 * <0 = load/parse error. `verbose` prints a summary. */
int replay_session_file(const char *path, int verbose);

#endif /* RECORD_H */
