# Emergent City — balance dashboard

A **static**, auto-refreshing web dashboard that charts the city's balance variables
over time. No build step, no framework, nothing to host but static files — it reads a
CSV the simulator writes (one row per game-day) and re-renders the graphs on a poll, so
you never hit refresh.

```
tools/dashboard/
├── index.html          the dashboard (self-contained; ~16 charts)
├── chart.umd.min.js    Chart.js v4, vendored locally (works offline / air-gapped)
└── metrics.csv         written by the sim at runtime (gitignored)
```

## 1. Make the sim write metrics

Both the GUI and the headless binary emit the same CSV — one row per game-day, every
aggregate the city tracks (population by life stage, births/deaths, economy, every crime
kind, knowledge/tech adoption, culture, factions/wars, socioeconomics — Gini, income
distribution, segregation, mobility, loot & crime by wealth — and **weather** temp/rain/fog
when `--weather` is on). Point the output at this directory so the web server serves the
same file the sim writes:

> The same per-day metrics also stream to **OpenSearch** (via Data Prepper) when a session
> is recorded with `CSIM_OS_INGEST_URL` set — see the "Record, replay & archive" section of
> the [top-level README](../../README.md).

```sh
# headless — record a 40-day run
./csim/build/csim_headless --days 40 --metrics tools/dashboard/metrics.csv

# GUI — live-feed while you watch the city
CSIM_METRICS=tools/dashboard/metrics.csv ./csim/build/csim
# (or: ./csim/build/csim --metrics tools/dashboard/metrics.csv)
```

Each run also writes `metrics.csv.meta` — the seed + every config knob — so the run can
be reproduced exactly (see **Replay** below).

## 2. Serve it

Any static file server works. Quick local check:

```sh
cd tools/dashboard && python3 -m http.server 8080
# open http://localhost:8080/
```

### nginx

Serve this directory and let the page poll `metrics.csv`:

```nginx
server {
    listen 80;
    server_name _;
    root /srv/emergent-city/dashboard;   # copy tools/dashboard/ here
    index index.html;

    # never cache the live data file (the page also cache-busts with ?t=)
    location = /metrics.csv {
        add_header Cache-Control "no-store";
    }
}
```

Reload nginx (`nginx -s reload`) and open the host in a browser. The sim can write
`metrics.csv` straight into `root` (e.g. `--metrics /srv/emergent-city/dashboard/metrics.csv`),
or you can `scp`/copy it there.

### Live instance (home-ubuntu, 10.0.0.134)

Deployed on the shared `nginx-unified` container (host networking) at
**http://10.0.0.134:8098/** — reboot-durable:

- web root: `/opt/docker/deploy/emergent-city/web/` on the host, bind-mounted into the
  container (`-v …:…:ro` in `docker-run.sh`); holds `index.html`, `chart.umd.min.js`,
  `metrics.csv`
- vhost: `/opt/docker/deploy/nginx-unified/conf.d/emergent-city.conf` (listen 8098)
- the mount is served **read-only**, so nginx reads the CSV while the sim (running as a
  normal user on the host) writes it — no permission clash
- **revert:** delete the conf file + the `-v` line in `docker-run.sh`, then recreate the
  container (`docker rm -f nginx-unified && docker-run.sh`); `docker-run.sh.bak-*` backups exist
- **feed it (easiest):** [`feed.sh`](feed.sh) runs the sim locally and live-mirrors the
  growing CSV to the web root over SSH (rsync loop, sends only the delta):
  ```sh
  tools/dashboard/feed.sh -- --days 40           # headless, live
  tools/dashboard/feed.sh --bin ./csim/build/csim --   # GUI, live
  tools/dashboard/feed.sh --mirror-only --local /tmp/m.csv   # mirror an existing writer
  ```
  Or feed it directly: write the CSV into the web root
  (`--metrics /opt/docker/deploy/emergent-city/web/metrics.csv` when running on the host),
  or `scp`/`rsync` your `metrics.csv` there.

> Recreating this shared container briefly drops every nginx-unified vhost, so only do it
> for mount changes; day-to-day edits (conf tweaks) just need `nginx -s reload`.

## 3. Auto-refresh & interaction

The page polls `metrics.csv` every **1.5 s** (override with `?poll=3000` for 3 s),
cache-busts each request, and updates the Chart.js datasets in place — no reload, no
flicker. The header dot shows **live** while rows are still arriving and **idle** once
the file stops growing. Load a different file with `?src=other.csv`.

**Click any chart to enlarge** it in a modal (it keeps updating live while open; close
with ×, the backdrop, or Esc).

## 3b. Sampling resolution (finer than per-day)

By default the sim writes **one row per game-day**. For smoother graphs, sample more
often with `--metrics-every H` (game-hours) or `--metrics-hourly` (= `--metrics-every 1`):

```sh
./csim/build/csim_headless --days 10 --metrics-hourly --metrics tools/dashboard/metrics.csv
./csim/build/csim_headless --days 10 --metrics-every 0.5 --metrics .../metrics.csv   # twice/hour
```

The CSV carries a fractional-day column **`t`** (`day + hour/24`) plus `day` and `hour`;
the dashboard uses `t` as the x-axis automatically when present, so intra-day rows plot
correctly. Cumulative counters (deaths, crimes, …) accumulate between samples; the
"per-day" delta charts become per-**sample** deltas at finer cadence. The cadence is
recorded in `*.meta`, so `--rerun` reproduces it.

## 4. Replay a previous session (headless)

Two ways to re-view a past run, both feeding this same dashboard:

```sh
# (a) PLAYBACK — stream a recorded CSV back out row-by-row so the graphs animate it
#     (pure playback, no simulation; --replay-interval sets seconds per day)
./csim/build/csim_headless --replay old.csv \
    --metrics tools/dashboard/metrics.csv --replay-interval 0.3

# (b) DETERMINISTIC RE-RUN — reproduce the run from its manifest; the regenerated CSV
#     matches the original byte-for-byte (same seed + config)
./csim/build/csim_headless --rerun old.csv.meta --metrics tools/dashboard/metrics.csv
```

> Re-run reproduces byte-for-byte only with the **LLM off** (`OPENROUTER_*` unset) — an
> async model consult makes any run non-deterministic. This is the same reproducibility
> rule as the rest of the sim; playback (a) is unaffected since it never simulates.

In both cases, open the dashboard and watch the charts fill in as the file grows.
