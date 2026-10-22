#!/usr/bin/env python3
"""Perception half of the rig harness: screenshot -> decision model answers.

No UI knowledge lives here. Callers build an ``ask`` mapping of question name
to spec and get a normalized result back; profiles define the screens, choices
and predicates for their game.

Question specs::

    {"type": "choice", "instructions": "...",
     "choices": [{"value": "title", "description": "..."}],       # Luna
     "criteria": {"title": "..."}}                                # Mercury
    {"type": "predicate", "instructions": "..."}     # alias: "noul"

A choice spec may give ``choices``, ``criteria`` or both; the missing form is
derived. ``ask_decisions`` returns::

    {"answers": {name: {"choice", "confidence"} | {"probability"}},
     "raw": {name: <answer object as returned by the API>},
     "screenshot": path, "latency": seconds, "backend": ..., "model": ...,
     "usage": {"input_tokens", "output_tokens", "cost"}}

``degrade`` and ``low_confidence`` implement the confidence policy. ``Trace``
writes the one-JSON-line-per-step record used by the rig (same field names as
the original driver). ``TailOracle`` watches a log file for new bytes only.
"""
import base64
import json
import os
import time
import urllib.error
import urllib.request

OPENROUTER_URL = "https://openrouter.ai/api/alpha/decisions"
LUNA_URL = "https://api.experientiallabs.ai/v1/decisions"
MERCURY_MODEL = "inception/mercury-decide:free"
LUNA_MODEL = "gpt-6-luna-decisions"
BACKENDS = ("mercury", "luna")
DEFAULT_BACKEND = "mercury"
DEFAULT_PROMPT = "Screenshot from a test rig. Answer every question about this frame only."


# ---- keys (never logged) ----
def openrouter_key():
    key = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if key:
        return key
    path = os.environ.get("OPENROUTER_ENV_FILE")
    if path:
        try:
            with open(os.path.expanduser(path), encoding="utf-8") as env_file:
                for line in env_file:
                    name, _, value = line.partition("=")
                    if name.strip().removeprefix("export ").strip() != "OPENROUTER_API_KEY":
                        continue
                    key = value.strip().strip("'\"")
                    if key:
                        return key
        except OSError as exc:
            raise RuntimeError(f"OPENROUTER_ENV_FILE unreadable: {exc}") from exc
    raise RuntimeError("set OPENROUTER_API_KEY or OPENROUTER_ENV_FILE")


def explabs_key():
    key = os.environ.get("EXPLABS_API_KEY", "").strip()
    if key:
        return key
    path = os.environ.get("EXPLABS_KEY_FILE")
    if path:
        try:
            with open(os.path.expanduser(path), encoding="utf-8") as key_file:
                key = key_file.read().strip()
        except OSError as exc:
            raise RuntimeError(f"EXPLABS_KEY_FILE unreadable: {exc}") from exc
        if key:
            return key
    raise RuntimeError("set EXPLABS_API_KEY or EXPLABS_KEY_FILE")


def api_key(backend):
    return openrouter_key() if backend == "mercury" else explabs_key()


# ---- HTTP ----
def _post(url, body, key, retries=3):
    data = json.dumps(body).encode()
    headers = {"Content-Type": "application/json", "Authorization": "Bearer " + key}
    last = None
    for attempt in range(retries):
        request = urllib.request.Request(url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as exc:
            detail = exc.read()[:400].decode(errors="replace")
            last = RuntimeError(f"decision API HTTP {exc.code}: {detail}")
            if exc.code < 500 or attempt == retries - 1:
                raise last
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last = RuntimeError(f"decision API request failed: {exc}")
            if attempt == retries - 1:
                raise last
        time.sleep(1.5)
    raise last


def _normalize(ask):
    questions = {}
    for name, spec in ask.items():
        kind = spec.get("type", "choice")
        instructions = spec.get("instructions", "")
        if kind in ("predicate", "noul"):
            questions[name] = {"type": "predicate", "instructions": instructions}
            continue
        if kind != "choice":
            raise ValueError(f"question {name!r}: unknown type {kind!r}")
        choices = spec.get("choices")
        criteria = spec.get("criteria")
        if choices is None:
            if not criteria:
                raise ValueError(f"question {name!r}: choice needs choices or criteria")
            choices = [{"value": value, "description": text}
                       for value, text in criteria.items()]
        if criteria is None:
            criteria = {item["value"]: item.get("description", "") for item in choices}
        questions[name] = {"type": "choice", "instructions": instructions,
                           "choices": choices, "criteria": dict(criteria)}
    return questions


def _collect_answers(payload):
    """Find answer objects regardless of the API's envelope."""
    found = {}

    def walk(node):
        if isinstance(node, dict):
            if isinstance(node.get("name"), str) and node.get("type") in (
                    "choice", "predicate", "refusal"):
                found.setdefault(node["name"], node)
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(payload)
    return found


def _usage(payload):
    usage = payload.get("usage") or {}
    return {
        "input_tokens": int(usage.get("input_tokens") or 0),
        "output_tokens": int(usage.get("output_tokens") or 0),
        "cost": float(usage.get("cost") or 0.0),
    }


def ask_decisions(image_path, ask, backend=DEFAULT_BACKEND, model=None, *,
                  prompt=None, key=None, retries=3):
    """Ask one decision request about ``image_path``; return normalized answers."""
    if backend not in BACKENDS:
        raise ValueError(f"unknown backend {backend!r}; use one of {BACKENDS}")
    model = model or (MERCURY_MODEL if backend == "mercury" else LUNA_MODEL)
    key = key or api_key(backend)
    prompt = prompt or DEFAULT_PROMPT
    questions = _normalize(ask)
    with open(image_path, "rb") as image:
        encoded = base64.b64encode(image.read()).decode()

    started = time.monotonic()
    if backend == "mercury":
        body = {
            "model": model,
            "state": [prompt, {"type": "image_url",
                               "image_url": {"url": "data:image/png;base64," + encoded}}],
            "questions": {
                name: ({"type": "choice", "instructions": spec["instructions"],
                        "criteria": spec["criteria"]}
                       if spec["type"] == "choice" else
                       {"type": "noul", "instructions": spec["instructions"]})
                for name, spec in questions.items()
            },
        }
        payload = _post(OPENROUTER_URL, body, key, retries)
        returned = payload.get("answers")
        if not isinstance(returned, dict):
            raise RuntimeError("Mercury Decide response missing answers")
        answers, raw = {}, {}
        for name, spec in questions.items():
            answer = returned.get(name)
            if not isinstance(answer, dict):
                raise RuntimeError(f"Mercury Decide response missing answer {name!r}")
            raw[name] = dict(answer)
            if spec["type"] == "choice":
                if not isinstance(answer.get("choice"), str):
                    raise RuntimeError(
                        f"Mercury Decide response missing choice {name!r}")
                answers[name] = {"choice": answer["choice"],
                                 "confidence": float(answer.get("confidence") or 0.0)}
            else:
                if "noul" not in answer:
                    raise RuntimeError(
                        f"Mercury Decide response missing noul {name!r}")
                answers[name] = {"probability": float(answer["noul"])}
    else:
        body = {
            "model": model,
            "input": [{"role": "user", "content": [
                {"type": "input_text", "text": prompt},
                {"type": "input_image",
                 "image_url": "data:image/png;base64," + encoded},
            ]}],
            "questions": [
                ({"type": "choice", "name": name, "instructions": spec["instructions"],
                  "choices": spec["choices"]}
                 if spec["type"] == "choice" else
                 {"type": "predicate", "name": name,
                  "instructions": spec["instructions"]})
                for name, spec in questions.items()
            ],
        }
        payload = _post(LUNA_URL, body, key, retries)
        found = _collect_answers(payload)
        answers, raw = {}, {}
        for name, spec in questions.items():
            answer = found.get(name)
            if answer is None:
                raise RuntimeError(f"Decisions response missing answer {name!r}")
            if answer.get("type") == "refusal":
                raise RuntimeError(f"model refused question {name!r}")
            raw[name] = dict(answer)
            if spec["type"] == "choice":
                answers[name] = {"choice": answer.get("choice"),
                                 "confidence": float(answer.get("confidence") or 0.0)}
            else:
                answers[name] = {"probability": float(answer.get("probability", 0.0))}
    latency = time.monotonic() - started
    return {
        "answers": answers,
        "raw": raw,
        "screenshot": image_path,
        "latency": latency,
        "backend": backend,
        "model": model,
        "usage": _usage(payload),
    }


# ---- generic per-frame perception ----
def profile_ask(profile):
    """Question spec from a profile's screen/highlight/predicate sections."""
    return {
        "screen": {"type": "choice", "instructions": profile["screen_instructions"],
                   "choices": profile["screens"]},
        "highlight": {"type": "choice",
                      "instructions": profile["highlight_instructions"],
                      "choices": profile["highlights"]},
        **profile["predicates"],
    }


class Perceiver:
    """Capture -> ask -> confidence policy, for any profile's question set.

    ``capture(name, settle)`` returns an image path (or None); ``ask`` is the
    question spec mapping. Each choice question lands on the state as
    ``state[name]`` plus ``state[name + "_confidence"]``; each predicate as
    ``state[name]``. ``fallbacks`` maps a choice name to what a low-confidence
    answer degrades to (eg screen -> other, highlight -> uncertain).
    """

    def __init__(self, capture, ask, backend=DEFAULT_BACKEND, model=None, *,
                 prompt=None, confidence_min=0.6, fallbacks=None,
                 prefix="frame", label=None, retry_tags=(("a", 0.25), ("b", 1.0))):
        self.capture = capture
        self.questions = _normalize(ask)
        self.backend = backend
        self.model = model
        self.prompt = prompt
        self.confidence_min = confidence_min
        self.fallbacks = fallbacks or {}
        self.prefix = prefix
        self.label = label or prefix
        self.retry_tags = retry_tags
        self.key = api_key(backend)

    @classmethod
    def from_profile(cls, profile, capture, backend=DEFAULT_BACKEND, model=None, *,
                     prefix="frame", label=None):
        safety = profile["safety"]
        return cls(capture, profile_ask(profile), backend, model,
                   prompt=profile.get("prompt"),
                   confidence_min=safety["confidence_min"],
                   fallbacks=profile.get("fallbacks", {}),
                   prefix=prefix, label=label)

    def perceive(self, step=0):
        result = self._ask(step, *self.retry_tags[0])
        low = self._low(result["answers"])
        if low and len(self.retry_tags) > 1:
            result = self._ask(step, *self.retry_tags[1])
            low = self._low(result["answers"])
        state = {"screenshot": result["screenshot"], "latency": result["latency"],
                 "backend": result["backend"], "model": result["model"],
                 "usage": result["usage"], "answers": {}, "raw": {}}
        for name, spec in self.questions.items():
            answer = result["answers"][name]
            if spec["type"] == "choice":
                confidence = round(float(answer.get("confidence", 0.0)), 3)
                raw = answer.get("choice")
                value = raw
                fallback = self.fallbacks.get(name)
                if name in low and fallback is not None:
                    value = fallback
                allowed = {item["value"] for item in spec["choices"]}
                if value not in allowed and value != fallback:
                    raise RuntimeError(f"unknown {name} choice {value!r}")
                state[name] = value
                state[f"{name}_confidence"] = confidence
                state["answers"][name] = {"choice": value, "confidence": confidence}
                state["raw"][name] = {"choice": raw, "confidence": confidence}
            else:
                probability = round(float(answer.get("probability", 0.0)), 3)
                state[name] = probability
                state["answers"][name] = probability
        return state

    def _low(self, answers):
        names = [name for name, spec in self.questions.items()
                 if spec["type"] == "choice"]
        return low_confidence(answers, names, self.confidence_min)

    def _ask(self, step, tag, settle):
        name = f"{self.prefix}-{time.strftime('%H%M%S')}-{step}-{tag}.png"
        path = self.capture(name, settle)
        if not path:
            raise RuntimeError(f"screenshot failed for {self.label}")
        return ask_decisions(path, self.questions, self.backend, self.model,
                             prompt=self.prompt, key=self.key)


# ---- confidence policy ----
def low_confidence(answers, names, minimum):
    """Names whose choice confidence is below ``minimum``."""
    return [name for name in names
            if float(answers[name].get("confidence", 0.0)) < minimum]


def degrade(choice, confidence, minimum, fallback):
    """A low-confidence choice becomes ``fallback`` (e.g. other/uncertain)."""
    return fallback if float(confidence) < minimum else choice


# ---- trace ----
class Trace:
    """One JSONL line per step, same field names as the original driver."""

    def __init__(self, directory=None, prefix="coop"):
        directory = directory or os.environ.get("RIG_SHOTS") or "."
        os.makedirs(directory, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        path = os.path.join(directory, f"{prefix}-{stamp}.jsonl")
        suffix = 1
        while os.path.exists(path):
            path = os.path.join(directory, f"{prefix}-{stamp}-{suffix}.jsonl")
            suffix += 1
        self.path = path
        self.n = 0
        self._file = open(path, "w", encoding="utf-8")

    def write(self, **record):
        self.n += 1
        record["step"] = self.n
        self._file.write(json.dumps(record, sort_keys=True) + "\n")
        self._file.flush()

    def close(self):
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ---- log oracle ----
class TailOracle:
    """Only bytes written after construction count as new evidence."""

    def __init__(self, path):
        self.path = path
        try:
            self.pos = os.path.getsize(path)
        except OSError:
            self.pos = 0
        self.text = ""

    def scan(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return ""
        if size < self.pos:
            self.pos = 0
        if size == self.pos:
            return ""
        try:
            with open(self.path, "rb") as log:
                log.seek(self.pos)
                chunk = log.read(size - self.pos)
        except OSError:
            return ""
        self.pos += len(chunk)
        new = chunk.decode(errors="replace")
        self.text += new
        return new

    def found(self, needle):
        return needle in self.text


class NeedleOracle(TailOracle):
    """A TailOracle pinned to one needle."""

    def __init__(self, path, needle):
        super().__init__(path)
        self.needle = needle

    def found(self):
        return super().found(self.needle)
