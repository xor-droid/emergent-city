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
kind, knowledge/tech adoption, culture, factions/wars …). Point the output at this
directory so the web server serves the same file the sim writes:

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
**http://10.0.0.134:8098/** — zero-downtime vhost, no container recreate:

- web root: `/tmp/emergent-city-dash/` on the host (already bind-mounted into the
  container via `-v /tmp:/tmp`); holds `index.html`, `chart.umd.min.js`, `metrics.csv`
- vhost: `/opt/docker/deploy/nginx-unified/conf.d/emergent-city.conf` (listen 8098)
- added with: `docker exec nginx-unified nginx -t && docker exec nginx-unified nginx -s reload`
- **revert:** delete that conf file + reload
- **feed it:** write the CSV into the web root, e.g.
  `./csim/build/csim_headless --days 40 --metrics /tmp/emergent-city-dash/metrics.csv`
  run on the host, or `scp` your `metrics.csv` there.

> `/tmp` is wiped on reboot. For a reboot-durable root, move the files to
> `/opt/docker/deploy/emergent-city/web`, add a matching `-v` bind mount in
> `/opt/docker/deploy/nginx-unified/docker-run.sh`, and recreate the container.

## 3. Auto-refresh

The page polls `metrics.csv` every **1.5 s** (override with `?poll=3000` for 3 s),
cache-busts each request, and updates the Chart.js datasets in place — no reload, no
flicker. The header dot shows **live** while rows are still arriving and **idle** once
the file stops growing. Load a different file with `?src=other.csv`.

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
