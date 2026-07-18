"""soundBud backend — voice in, generated music out.

    POST /utterance   raw 16kHz mono WAV  ->  {say, screen, audio_url, volume}
    GET  /tracks/*.mp3                        the generated audio

Pipeline: transcribe -> Claude decides what the user meant -> generate if needed.

Run:  uvicorn main:app --host 0.0.0.0 --port 8000
      (0.0.0.0, not 127.0.0.1 — the device is a different machine)
"""

import os
import time
from pathlib import Path
from typing import Literal

import anthropic
import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, Request
from fastapi.staticfiles import StaticFiles
from fastmcp import FastMCP
from pydantic import BaseModel

# Keys live in .env at the repo root (gitignored). Copy .env.example to start.
load_dotenv(Path(__file__).parent.parent / ".env")

ELEVEN = "https://api.elevenlabs.io/v1"

# Cheapest Anthropic model that still does structured outputs. Intent
# classification is an easy task and this call sits in the latency path.
MODEL = "claude-haiku-4-5"
TRACKS = Path(__file__).parent / "tracks"
TRACKS.mkdir(exist_ok=True)

# Music generation is slow. This must be generous or we give up on a request
# that was going to succeed.
GENERATION_TIMEOUT = 180.0

# How a remote agent should reach us for the mp3s. The MCP tool has no request
# to read a host off, unlike /utterance. Set this to your LAN address.
PUBLIC_URL = os.getenv("SOUNDBUD_URL", "http://localhost:8000").rstrip("/")

claude = anthropic.Anthropic()

# Mounted on the same app so there is one process, one port, one firewall rule.
mcp = FastMCP("soundbud")
mcp_app = mcp.http_app(path="/")

app = FastAPI(lifespan=mcp_app.lifespan)
app.mount("/tracks", StaticFiles(directory=TRACKS), name="tracks")
app.mount("/mcp", mcp_app)


# ─── what the AI decides ────────────────────────────────────────────────────

class TrackSpec(BaseModel):
    prompt: str          # the music description sent to ElevenLabs
    duration_ms: int
    instrumental: bool


class Plan(BaseModel):
    # "quieter" is ambiguous — playback volume, or calmer music? Deciding this
    # is the whole job. Getting it wrong is what makes the device feel broken.
    intent: Literal["set_volume", "modify_track", "new_track", "transport"]
    volume: float | None   # absolute target 0..1, only for set_volume
    track: TrackSpec | None  # only for modify_track / new_track
    say: str               # spoken/《logged》 reply, covers generation latency
    screen: str            # <=20 chars, the display is tiny


SYSTEM = """You control a voice-driven music station. The user speaks; you decide
what they meant and reply with a Plan.

Choosing the intent is the most important thing you do:

- set_volume: words about LOUDNESS. "quieter", "turn it down", "too loud",
  "louder". Return an absolute `volume` 0..1 based on the current volume.
  This is instant and free.
- modify_track: words about MOOD, ENERGY, INSTRUMENTS, or GENRE applied to what
  is already playing. "calmer", "softer", "more energetic", "add drums",
  "less busy". Return the CURRENT track spec with its prompt adjusted — keep
  everything the user did not ask to change. This regenerates and is slow.
- new_track: a fresh request unrelated to what's playing.
- transport: "skip", "next", "stop", "pause".

If genuinely ambiguous between set_volume and modify_track, prefer set_volume:
it is instant, free, and trivially corrected.

`screen` must be at most 20 characters — it goes on a 160x80 display.
`say` is one short friendly sentence; it is played while music generates, so it
should acknowledge what is coming."""


# ─── session state ──────────────────────────────────────────────────────────
# ponytail: module-level globals. One device, one listener, one conversation.
# Move to a dict keyed by device id when there is a second station.

current_track: TrackSpec | None = None
volume: float = 0.6


# ─── steps ──────────────────────────────────────────────────────────────────

def transcribe(wav: bytes) -> str:
    r = httpx.post(
        f"{ELEVEN}/speech-to-text",
        headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"]},
        files={"file": ("utterance.wav", wav, "audio/wav")},
        data={"model_id": "scribe_v1"},
        timeout=60.0,
    )
    r.raise_for_status()
    return r.json()["text"].strip()


def plan_from(utterance: str) -> Plan:
    context = (
        f"Currently playing: {current_track.model_dump_json()}"
        if current_track
        else "Nothing is playing yet."
    )
    response = claude.messages.parse(
        model=MODEL,
        max_tokens=1024,
        system=SYSTEM,
        messages=[{
            "role": "user",
            "content": f"{context}\nCurrent volume: {volume:.2f}\n\nUser said: {utterance}",
        }],
        output_format=Plan,
    )
    return response.parsed_output


def generate(spec: TrackSpec) -> str:
    """Generate a track, save it, return its filename."""
    started = time.monotonic()
    r = httpx.post(
        f"{ELEVEN}/music",
        headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"]},
        json={
            "prompt": spec.prompt,
            "music_length_ms": spec.duration_ms,
            "model_id": "music_v1",
            "force_instrumental": spec.instrumental,
        },
        params={"output_format": "mp3_44100_128"},
        timeout=GENERATION_TIMEOUT,
    )
    r.raise_for_status()

    name = f"track_{int(time.time())}.mp3"
    (TRACKS / name).write_bytes(r.content)
    print(f"generated {name} in {time.monotonic() - started:.1f}s "
          f"({len(r.content) // 1024}KB) :: {spec.prompt}")
    return name


@mcp.tool
def generate_music(prompt: str, duration_seconds: int = 30,
                   instrumental: bool = True) -> str:
    """Generate a music track and return a URL to the mp3.

    `prompt` describes the music: genre, mood, instruments, tempo. Be specific —
    "slow lo-fi hip hop, warm rhodes, vinyl crackle" beats "chill music".
    Takes up to a couple of minutes. Duration is clamped to 10..300 seconds.
    """
    seconds = min(300, max(10, duration_seconds))
    spec = TrackSpec(prompt=prompt, duration_ms=seconds * 1000,
                     instrumental=instrumental)
    return f"{PUBLIC_URL}/tracks/{generate(spec)}"


def apply(plan: Plan, track: TrackSpec | None, vol: float):
    """Fold a Plan into session state. Pure — no network, no globals.

    Returns (track, volume, needs_generation).
    """
    if plan.intent == "set_volume":
        # Claude is asked for an absolute target but is not schema-constrained
        # to 0..1, so clamp rather than trust.
        vol = min(1.0, max(0.0, plan.volume if plan.volume is not None else vol))
        return track, vol, False

    if plan.intent == "transport":
        return track, vol, False

    if plan.track is None:
        return track, vol, False  # asked to generate but gave no spec

    # ponytail: "make it calmer" re-rolls a fresh song rather than adjusting
    # the current one — the API refuses `seed` alongside `prompt`, so there is
    # no continuity knob. Revisit if ElevenLabs ships one.
    return plan.track, vol, True


# ─── endpoint ───────────────────────────────────────────────────────────────

@app.get("/")
def health():
    """Open this from a phone browser to prove network + firewall before
    blaming the firmware."""
    return {"ok": True, "playing": current_track.prompt if current_track else None,
            "volume": volume}


@app.post("/utterance")
async def utterance(request: Request):
    global current_track, volume

    wav = await request.body()
    text = transcribe(wav)
    print(f"heard: {text!r}")

    plan = plan_from(text)
    print(f"intent: {plan.intent}")

    current_track, volume, needs_generation = apply(plan, current_track, volume)

    audio_url = None
    if needs_generation:
        name = generate(current_track)
        # base_url is whatever host the device reached us on, so this is
        # already the right LAN address without configuring it anywhere.
        audio_url = f"{str(request.base_url).rstrip('/')}/tracks/{name}"

    return {
        "say": plan.say,
        "screen": plan.screen[:20],
        "audio_url": audio_url,
        "volume": round(volume, 2),
    }


if __name__ == "__main__":
    # Smallest check that fails if the state logic breaks. No network needed.
    chill = TrackSpec(prompt="chill lo-fi", duration_ms=30000, instrumental=True)

    t, v, gen = apply(
        Plan(intent="new_track", volume=None, track=chill, say="", screen=""),
        None, 0.6)
    assert gen and t == chill

    warmer = chill.model_copy(update={"prompt": "chill lo-fi, warmer"})
    t2, _, gen2 = apply(
        Plan(intent="modify_track", volume=None, track=warmer, say="", screen=""),
        t, v)
    assert gen2 and t2.prompt.endswith("warmer")

    # volume never generates, and is clamped
    _, v3, gen3 = apply(
        Plan(intent="set_volume", volume=1.9, track=None, say="", screen=""),
        t2, v)
    assert not gen3 and v3 == 1.0

    print("ok")
