"""
openrouter_client.py — Thin wrapper around an OpenAI-compatible
chat-completions API.

Works against either OpenRouter (cloud) or a local OpenAI-compatible server
such as llama.cpp's `llama-server` (e.g. a local Qwen3 GGUF). The endpoint is
selected via `base_url` (constructor arg or the OPENROUTER_BASE_URL env var);
it defaults to OpenRouter for backwards compatibility.

Async-friendly via a thread pool; call submit() for fire-and-forget.
"""

from __future__ import annotations
import os
import time
from dataclasses import dataclass
from typing import List, Dict, Optional
from concurrent.futures import ThreadPoolExecutor, Future

import requests

import config


DEFAULT_OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"


@dataclass
class LLMResponse:
    text: str
    model: str
    tokens_in: int = 0
    tokens_out: int = 0
    latency_ms: float = 0.0
    error: Optional[str] = None


class OpenRouterClient:
    """Synchronous-but-pooled OpenAI-compatible client.

    Despite the name, this talks to any OpenAI-compatible /chat/completions
    endpoint. Use submit() for fire-and-forget from the game loop.
    """

    def __init__(self,
                 api_key: Optional[str] = None,
                 model: Optional[str] = None,
                 site_url: str = "",
                 app_name: str = "EmergentCity",
                 base_url: Optional[str] = None) -> None:
        self.api_key = (api_key if api_key is not None
                        else os.environ.get("OPENROUTER_API_KEY", ""))
        self.model = model or os.environ.get("OPENROUTER_MODEL", "")
        self.site_url = site_url or os.environ.get("OPENROUTER_SITE_URL", "")
        self.app_name = app_name or os.environ.get("OPENROUTER_APP_NAME", "EmergentCity")
        self.base_url = (base_url
                         or os.environ.get("OPENROUTER_BASE_URL", "").strip()
                         or getattr(config, "LLM_BASE_URL", "").strip()
                         or DEFAULT_OPENROUTER_URL)
        self.pool = ThreadPoolExecutor(max_workers=config.LLM_POOL_SIZE)
        self.total_calls = 0
        self.total_tokens = 0
        self.failures = 0

    @property
    def enabled(self) -> bool:
        return bool(self.api_key) and config.LLM_ENABLED

    def submit(self, messages: List[Dict[str, str]],
               model: Optional[str] = None,
               max_tokens: int = 200,
               temperature: float = 0.85) -> Future:
        return self.pool.submit(self.complete, messages, model, max_tokens, temperature)

    def _prepare_messages(self, messages: List[Dict[str, str]]) -> List[Dict[str, str]]:
        """Optionally disable Qwen3-style chain-of-thought.

        Qwen3 defaults to a "thinking" mode that emits a long reasoning
        preamble and leaves `content` empty when the token budget is tight
        (see config.LLM_DISABLE_THINKING). Appending the `/no_think` soft
        switch keeps replies short and in the `content` field.
        """
        if not config.LLM_DISABLE_THINKING:
            return messages
        out = [dict(m) for m in messages]
        for m in out:
            if m.get("role") == "system":
                m["content"] = f"{m['content']} /no_think"
                return out
        # No system message — append the switch to the last user turn.
        if out:
            out[-1]["content"] = f"{out[-1]['content']} /no_think"
        return out

    @staticmethod
    def _extract_text(message: Dict) -> str:
        """Return assistant text, falling back to reasoning_content.

        Some reasoning models (Qwen3) may place the answer in
        `reasoning_content` and leave `content` empty.
        """
        content = (message.get("content") or "").strip()
        if content:
            return content
        return (message.get("reasoning_content") or "").strip()

    def complete(self, messages: List[Dict[str, str]],
                 model: Optional[str] = None,
                 max_tokens: int = 200,
                 temperature: float = 0.85) -> LLMResponse:
        if not self.enabled:
            return LLMResponse(text="", model="disabled", error="LLM disabled")

        chosen = model or self.model
        models_to_try = [chosen] if chosen else list(getattr(config, "LLM_MODELS", []))
        prepared = self._prepare_messages(messages)
        last_err = None

        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        # OpenRouter analytics headers (harmless for local servers).
        if self.site_url:
            headers["HTTP-Referer"] = self.site_url
        if self.app_name:
            headers["X-Title"] = self.app_name

        for m in models_to_try:
            if not m:
                continue
            t0 = time.time()
            try:
                resp = requests.post(
                    self.base_url,
                    headers=headers,
                    json={
                        "model": m,
                        "messages": prepared,
                        "max_tokens": max_tokens,
                        "temperature": temperature,
                    },
                    timeout=config.LLM_TIMEOUT_SECONDS,
                )
                latency = (time.time() - t0) * 1000.0
                if resp.status_code != 200:
                    last_err = f"{resp.status_code}: {resp.text[:200]}"
                    self.failures += 1
                    continue
                data = resp.json()
                text = self._extract_text(data["choices"][0]["message"])
                usage = data.get("usage", {}) or {}
                self.total_calls += 1
                self.total_tokens += usage.get("total_tokens", 0)
                return LLMResponse(
                    text=text, model=m,
                    tokens_in=usage.get("prompt_tokens", 0),
                    tokens_out=usage.get("completion_tokens", 0),
                    latency_ms=latency,
                )
            except Exception as e:  # noqa: BLE001
                last_err = str(e)
                self.failures += 1
                continue

        return LLMResponse(text="", model="failed", error=last_err or "no models")

    def shutdown(self) -> None:
        self.pool.shutdown(wait=False)
