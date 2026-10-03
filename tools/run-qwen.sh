#!/usr/bin/env bash
#
# run-qwen.sh — (re)create the emergent-qwen llama.cpp container on the GPU host.
#
# Reusable record of how the Qwen server is launched (there was no compose file).
# Run on the host with the GPU (e.g. 10.0.0.134). Recreating briefly drops the
# server (~10s model reload) — agents fall back to rule-based logic meanwhile.
#
# Tunables (env):
#   PARALLEL   number of server slots / concurrent requests (default 2).
#              --ctx-size is the TOTAL KV budget split across slots, so 8192/2
#              = 4096 ctx per slot (ample for this app) with no extra VRAM.
#   CTX        total context size (default 8192).
#   PORT       host port (default 9090).
#   MODEL      -hf model id (default Qwen/Qwen3-8B-GGUF:Q4_K_M).

set -euo pipefail

NAME="emergent-qwen"
IMAGE="ghcr.io/ggml-org/llama.cpp:server-cuda"
PARALLEL="${PARALLEL:-2}"
CTX="${CTX:-8192}"
PORT="${PORT:-9090}"
MODEL="${MODEL:-Qwen/Qwen3-8B-GGUF:Q4_K_M}"
HF_CACHE="${HF_CACHE:-$HOME/.cache/huggingface}"

echo ">> Removing existing $NAME (if any)..."
docker rm -f "$NAME" >/dev/null 2>&1 || true

echo ">> Starting $NAME with --parallel $PARALLEL (ctx $CTX, $((CTX/PARALLEL))/slot)..."
docker run -d \
  --name "$NAME" \
  --restart unless-stopped \
  --gpus all \
  -p "${PORT}:${PORT}" \
  -v "${HF_CACHE}:/root/.cache/huggingface" \
  -e LLAMA_ARG_HOST=0.0.0.0 \
  "$IMAGE" \
  -hf "$MODEL" \
  --host 0.0.0.0 --port "$PORT" \
  --ctx-size "$CTX" \
  --gpu-layers all \
  --flash-attn on \
  --cache-type-k q8_0 --cache-type-v q8_0 \
  --parallel "$PARALLEL"

echo ">> Waiting for the server to become healthy..."
for i in $(seq 1 60); do
  if curl -fsS -m 2 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
    echo ">> Healthy. Slots: $(curl -fsS "http://127.0.0.1:${PORT}/slots" 2>/dev/null | grep -o '"id"' | wc -l)"
    exit 0
  fi
  sleep 1
done
echo "!! Server did not become healthy in 60s — check: docker logs $NAME" >&2
exit 1
