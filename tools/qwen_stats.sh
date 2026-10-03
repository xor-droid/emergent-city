#!/usr/bin/env bash
#
# qwen_stats.sh — sample runtime stats on the Qwen host (RTX 3060 Ti) while the
# Emergent City sim is running, so the data can be fed back for review.
#
# Run this ON 10.0.0.134 (where the llama.cpp/Qwen container lives), start it
# just before you run the sim, and Ctrl+C it when you're done.
#
# Usage:
#   ./qwen_stats.sh [interval_seconds] [duration_seconds] [output.csv]
#   # interval default 2s, duration 0 = until Ctrl+C, output auto-named+timestamped
#
# Optional env overrides:
#   GPU_INDEX=0                 which GPU to sample
#   SERVER=http://127.0.0.1:9090   llama.cpp server base URL (health probe)
#   CONTAINER=emergent-qwen     if set (and docker present), also log the
#                               container's CPU/mem. NOTE: `docker stats` adds
#                               ~1.5s per sample, so use interval >= 3 with it.
#
# Feed back for review: send the generated CSV (or paste its tail) into the
# Claude session. The summary line printed on exit is a quick glance.

set -u

INTERVAL="${1:-2}"
DURATION="${2:-0}"
OUT="${3:-qwen_stats_$(date +%Y%m%d_%H%M%S).csv}"
GPU_INDEX="${GPU_INDEX:-0}"
SERVER="${SERVER:-http://127.0.0.1:9090}"
CONTAINER="${CONTAINER:-}"

HEADER="timestamp,gpu_util_pct,mem_bw_util_pct,vram_used_mib,vram_total_mib,temp_c,power_w,gpu_clk_mhz,load1,mem_used_pct,health_ms,container_cpu_pct,container_mem"

have() { command -v "$1" >/dev/null 2>&1; }

if ! have nvidia-smi; then
  echo "WARNING: nvidia-smi not found — GPU columns will be blank." >&2
fi

sample() {
  local ts gu mbw vu vt tc pw clk load1 mem_used_pct hs health_ms ccpu cmem cs
  ts="$(date -Is)"

  gu=; mbw=; vu=; vt=; tc=; pw=; clk=
  if have nvidia-smi; then
    # CSV, no header, no units -> strip the ", " separators to spaces, then read.
    read -r gu mbw vu vt tc pw clk < <(
      nvidia-smi --query-gpu=utilization.gpu,utilization.memory,memory.used,memory.total,temperature.gpu,power.draw,clocks.sm \
                 --format=csv,noheader,nounits -i "$GPU_INDEX" 2>/dev/null | tr -d ',')
  fi

  load1="$(awk '{print $1}' /proc/loadavg 2>/dev/null)"
  mem_used_pct="$(free 2>/dev/null | awk '/Mem:/{printf "%.1f", $3/$2*100}')"

  health_ms=
  if have curl; then
    hs="$(curl -s -o /dev/null -w '%{time_total}' -m 5 "$SERVER/health" 2>/dev/null)"
    [ -n "$hs" ] && health_ms="$(awk -v t="$hs" 'BEGIN{printf "%.0f", t*1000}')"
  fi

  ccpu=; cmem=
  if [ -n "$CONTAINER" ] && have docker; then
    cs="$(docker stats --no-stream --format '{{.CPUPerc}};{{.MemUsage}}' "$CONTAINER" 2>/dev/null)"
    ccpu="${cs%%;*}"
    cmem="${cs#*;}"
    cmem="${cmem// /}"   # drop spaces so the CSV column stays single-field
  fi

  echo "$ts,$gu,$mbw,$vu,$vt,$tc,$pw,$clk,$load1,$mem_used_pct,$health_ms,$ccpu,$cmem"
}

summarize() {
  [ -f "$OUT" ] || return
  awk -F, 'NR>1{
      n++
      if($2!=""){gu+=$2; if($2+0>gumax)gumax=$2}
      if($4!=""){vu+=$4; if($4+0>vumax)vumax=$4}
      if($7!=""){pw+=$7; if($7+0>pwmax)pwmax=$7}
      if($11!=""){h+=$11; if($11+0>hmax)hmax=$11}
    }
    END{
      if(n>0)
        printf "SUMMARY: samples=%d  gpu_util avg=%.1f%% max=%.0f%%  vram avg=%.0f max=%.0f MiB  power avg=%.0f max=%.0f W  health avg=%.0f max=%.0f ms\n",
               n, gu/n, gumax, vu/n, vumax, pw/n, pwmax, h/n, hmax
      else
        printf "SUMMARY: no data rows\n"
    }' "$OUT"
}

on_exit() {
  echo >&2
  summarize >&2
  echo "Wrote: $OUT" >&2
  exit 0
}
trap on_exit INT TERM

echo "$HEADER" | tee "$OUT"

start="$(date +%s)"
while :; do
  sample | tee -a "$OUT"
  if [ "$DURATION" -gt 0 ]; then
    now="$(date +%s)"
    [ $((now - start)) -ge "$DURATION" ] && break
  fi
  sleep "$INTERVAL"
done

on_exit
