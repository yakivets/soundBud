"""soundBud backend — voice in, generated music out.

    POST /utterance   raw 16kHz mono WAV  ->  {say, screen, audio_url, volume}
    GET  /tracks/*.mp3                        the generated audio

Pipeline: transcribe -> Claude decides what the user meant -> generate if needed.

Run:  uvicorn main:app --host 0.0.0.0 --port 8000
      (0.0.0.0, not 127.0.0.1 — the device is a different machine)
"""

import os
import time
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
from typing import Literal

import anthropic
import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
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

# Set SOUNDBUD_AUDIO=0 to skip generation and return only the text description
# in `music`. Useful when testing against a device that has a screen but no
# speaker — saves a minute of waiting and the ElevenLabs credits.
GENERATE_AUDIO = os.getenv("SOUNDBUD_AUDIO", "1") == "1"

# 30s everywhere: the device streams rather than buffers, so length is not a
# memory problem, but every extra second is ElevenLabs credits and time the user
# spends waiting. Claude picks the duration and is not schema-constrained, so
# this is enforced, not requested.
MAX_DURATION_MS = 30_000
MIN_DURATION_MS = 30_000

# 8s of 16kHz mono PCM is ~256KB. 2MB leaves room for a longer clip without
# letting an unbounded body read chew all our memory.
MAX_UPLOAD_BYTES = 2_000_000
WAV_HEADER_BYTES = 44

# Stock ElevenLabs voice. flash_v2_5 because this sits directly in front of the
# user — quality matters less than getting a reply out while music generates.
VOICE_ID = os.getenv("SOUNDBUD_VOICE", "21m00Tcm4TlvDq8ikWAM")
TTS_MODEL = "eleven_flash_v2_5"

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


class Ambient(BaseModel):
    """What the sensor board reports. Every field optional — a half-built board
    posting only temperature is still useful."""
    temp_c: float | None = None
    humidity: float | None = None
    light: int | None = None       # raw ADC, calibrated here not on the device
    lat: float | None = None
    lon: float | None = None
    has_fix: bool = False


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

You are told the time of day and, when a sensor board is connected, the light
level and temperature of the room. Use it to fill in what the user left unsaid —
never to override them.

- "play some drum and bass" is specific. Give them drum and bass. The room does
  not get a vote.
- "play something", "surprise me", "match the mood", "something that fits" is
  where you lean on it: a dark cold evening wants something different from a
  bright warm morning.
- Never read the readings aloud in `say`. "Something warm for the evening" is
  good; "it is 19°C and dim so here is jazz" is not — nobody wants their
  thermostat narrating.

Weather is the strongest signal you get. Rain, fog and overcast skies want
something different from clear sun — lean into it for open-ended requests.
Indoor temperature and outdoor temperature are given separately and can disagree;
that is normal, and the outdoor one usually says more about the mood.

When you are given a location, work out where that is and let the region colour
open-ended requests: the same "play something" should lean flamenco guitar and
rumba in Spain, and something more UK — garage, trip hop, folk — in London. Treat
it as an accent on the music, not the whole brief, and never name the country in
`say`. A specific request still wins outright: "play some techno" is techno
everywhere.

`duration_ms` must be 30000.

`screen` must be at most 20 characters — it goes on a 160x80 display.

`say` is spoken aloud the moment you reply, and it is the only thing the user
hears until the music is ready. Write it to be heard, not read.

For new_track and modify_track, generation takes about ten seconds, so `say` has
two jobs: name back what they asked for, so they know you understood, and tell
them a track is being made.

Write a fresh line every time. These are four different shapes, to show the range
you have — copying one as a template is what makes a device feel canned, and the
user hears this many times a day:

  "Some jazz, on it. Should be about ten seconds."
  "Nice — building you something calmer right now."
  "Techno it is. Hang on while I put this together."
  "Adding drums. Won't be long."

Notice none of them are built the same way. Some lead with the genre, some with
the acknowledgement, some mention the wait and some just imply it.

For set_volume and transport nothing is being generated and the change already
happened, so never ask them to wait. Keep it to an acknowledgement:

  "Turned it down."
  "Skipping this one."

Do not mention prompts, models, generating, or the API. One or two short
sentences, spoken like a person, no emoji."""


# ─── session state ──────────────────────────────────────────────────────────
# ponytail: module-level globals. One device, one listener, one conversation.
# Move to a dict keyed by device id when there is a second station.

current_track: TrackSpec | None = None
volume: float = 0.6

# Music generation runs here while the spoken reply goes back to the device, so
# the user hears something within a couple of seconds instead of waiting out the
# whole generation in silence. One worker: one device, one track at a time.
_pool = ThreadPoolExecutor(max_workers=1)
pending: Future | None = None

# Last reading from the sensor board, and when it arrived. Stale readings are
# worse than none — a temperature from this morning is a lie about right now.
ambient: Ambient | None = None
ambient_at: float = 0.0
AMBIENT_MAX_AGE = 600.0   # 10 minutes; the board posts every 60s

# The LDR divider can be wired either way round. If the device reports "bright"
# in a dark room, flip this rather than rewiring or reflashing.
LIGHT_INVERTED = False

# Where the device lives, used when the GPS has no fix — which indoors is
# always. Set SOUNDBUD_PLACE="Barcelona, Spain" and regional flavour works with
# no GPS at all. A real fix overrides it.
HOME_PLACE = os.getenv("SOUNDBUD_PLACE", "")

# Open-Meteo: free, no API key. Geocoding turns SOUNDBUD_PLACE into coordinates
# once; the forecast is cached because weather does not change in the seconds
# between two voice commands, and this call sits in the latency path.
OPEN_METEO = "https://api.open-meteo.com/v1/forecast"
GEOCODE = "https://geocoding-api.open-meteo.com/v1/search"
WEATHER_TTL = 900.0     # 15 minutes
_geo_cache: dict[str, tuple[float, float, str]] = {}
_weather: tuple[float, str, float | None] = (0.0, "", None)   # (at, text, outside_c)

# WMO weather codes, collapsed to what matters for choosing music.
WMO = [
    (0, "clear"), (1, "mostly clear"), (2, "partly cloudy"), (3, "overcast"),
    (48, "foggy"), (57, "drizzling"), (67, "raining"), (77, "snowing"),
    (82, "heavy rain"), (86, "snowing"), (99, "thunderstorms"),
]


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


def locate() -> tuple[float, float, str] | None:
    """Coordinates and place name. A GPS fix wins; otherwise geocode the
    configured home once and remember it."""
    if ambient and ambient.has_fix and ambient.lat is not None:
        return ambient.lat, ambient.lon, ""
    if not HOME_PLACE:
        return None
    if HOME_PLACE not in _geo_cache:
        # The geocoder wants a bare city name — "London, UK" returns nothing.
        query = HOME_PLACE.split(",")[0].strip()
        try:
            r = httpx.get(GEOCODE, params={"name": query, "count": 1}, timeout=10.0)
            hit = r.json()["results"][0]
            _geo_cache[HOME_PLACE] = (hit["latitude"], hit["longitude"],
                                      f"{hit['name']}, {hit['country']}")
            print(f"geocoded {HOME_PLACE!r} -> {_geo_cache[HOME_PLACE]}")
        except Exception as exc:
            # Keep the name so regional flavour still works; only weather is lost.
            print(f"geocode failed for {HOME_PLACE!r}: {exc}")
            _geo_cache[HOME_PLACE] = (0.0, 0.0, HOME_PLACE)   # cached: don't retry per request
    return _geo_cache[HOME_PLACE]


def weather_now() -> str:
    """Outside conditions in plain English. Cached, and never fatal — losing the
    weather should degrade the vibe, not break the request."""
    global _weather
    fetched, text, _ = _weather
    if time.time() - fetched < WEATHER_TTL:
        return text

    here = locate()
    if here is None:
        return ""
    lat, lon, _name = here
    if not lat and not lon:      # geocode failed; we have a name but no coordinates
        return ""
    outside_c = None
    try:
        r = httpx.get(OPEN_METEO, params={
            "latitude": lat, "longitude": lon,
            "current": "temperature_2m,weather_code,wind_speed_10m"}, timeout=10.0)
        cur = r.json()["current"]
        code = cur["weather_code"]
        sky = next(word for limit, word in WMO if code <= limit)
        outside_c = cur["temperature_2m"]
        text = f"{sky} and {outside_c:.0f}°C outside"
        if cur["wind_speed_10m"] > 30:
            text += ", windy"
    except Exception as exc:
        print(f"weather lookup failed: {exc}")
        text = ""

    _weather = (time.time(), text, outside_c)
    return text


def describe_ambient() -> str:
    """Render the latest sensor reading as plain English.

    Words, not numbers: the model picks better music from "dim and cold" than
    from a raw ADC count it has to interpret.
    """
    hour = time.localtime().tm_hour
    part = ("night" if hour < 6 else "morning" if hour < 12
            else "afternoon" if hour < 18 else "evening")
    bits = [part]

    if ambient and time.time() - ambient_at < AMBIENT_MAX_AGE:
        if ambient.light is not None:
            level = 4095 - ambient.light if LIGHT_INVERTED else ambient.light
            bits.append("dark" if level < 500 else "dim" if level < 1500
                        else "bright" if level > 3000 else "normal light")
        if ambient.temp_c is not None:
            t = ambient.temp_c
            bits.append(f"{t:.0f}°C indoors, " + ("cold" if t < 16 else
                                                  "warm" if t > 24 else "mild"))
        if ambient.humidity is not None and ambient.humidity > 65:
            bits.append("humid")

    here = locate()
    if here:
        lat, lon, name = here
        bits.append(f"in {name}" if name else f"at {lat:.2f}, {lon:.2f}")

    outside = weather_now()
    if outside:
        bits.append(outside)

    return ", ".join(bits)


def plan_from(utterance: str) -> Plan:
    context = (
        f"Currently playing: {current_track.model_dump_json()}"
        if current_track
        else "Nothing is playing yet."
    )
    vibe = describe_ambient()
    context += f"\nRight now it is: {vibe}"
    # Logged on every decision so it is visible what the model actually saw,
    # not just what the sensor board last posted.
    print(f"vibe: {vibe}")
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
    length_ms = min(MAX_DURATION_MS, max(MIN_DURATION_MS, spec.duration_ms))
    r = httpx.post(
        f"{ELEVEN}/music",
        headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"]},
        json={
            "prompt": spec.prompt,
            "music_length_ms": length_ms,
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


def speak(text: str) -> str:
    """Voice a line of text, save it, return its filename."""
    started = time.monotonic()
    r = httpx.post(
        f"{ELEVEN}/text-to-speech/{VOICE_ID}",
        headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"]},
        json={"text": text, "model_id": TTS_MODEL},
        params={"output_format": "mp3_44100_128"},
        timeout=30.0,
    )
    r.raise_for_status()

    name = f"say_{int(time.time() * 1000)}.mp3"
    (TRACKS / name).write_bytes(r.content)
    print(f"spoke {name} in {time.monotonic() - started:.1f}s :: {text}")
    return name


@mcp.tool
def generate_music(prompt: str, duration_seconds: int = 30,
                   instrumental: bool = True) -> str:
    """Generate a music track and return a URL to the mp3.

    `prompt` describes the music: genre, mood, instruments, tempo. Be specific —
    "slow lo-fi hip hop, warm rhodes, vinyl crackle" beats "chill music".
    Takes up to a minute. Duration is fixed at 30 seconds.
    """
    spec = TrackSpec(prompt=prompt, duration_ms=duration_seconds * 1000,
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
    fresh = ambient is not None and time.time() - ambient_at < AMBIENT_MAX_AGE
    return {"ok": True, "playing": current_track.prompt if current_track else None,
            "volume": volume,
            # Open this from a phone to see whether the sensor board is landing.
            "vibe": describe_ambient(),
            "sensors": ambient.model_dump() if fresh else None,
            "sensors_age_s": round(time.time() - ambient_at) if ambient else None}


@app.post("/utterance")
async def utterance(request: Request):
    global current_track, volume

    # Validate at the boundary: a device sending junk should get a clear 4xx,
    # not an opaque 500 from deep inside the ElevenLabs call.
    wav = await request.body()
    if len(wav) > MAX_UPLOAD_BYTES:
        raise HTTPException(413, f"body over {MAX_UPLOAD_BYTES} bytes")
    if len(wav) < WAV_HEADER_BYTES or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise HTTPException(400, "expected raw WAV bytes as the request body")

    text = transcribe(wav)
    print(f"heard: {text!r}")

    plan = plan_from(text)
    print(f"intent: {plan.intent}")

    current_track, volume, needs_generation = apply(plan, current_track, volume)

    # What the track would be, in words. Useful on the screen while there is no
    # speaker, and useful for debugging once there is one.
    music = current_track.prompt if needs_generation else None

    # base_url is whatever host the device reached us on, so URLs are already the
    # right LAN address without configuring it anywhere.
    base = str(request.base_url).rstrip("/")

    # Start the music first so it generates while the reply is being voiced.
    global pending
    generating = needs_generation and GENERATE_AUDIO
    if generating:
        track = current_track
        pending = _pool.submit(lambda: generate(track))
    elif music:
        print(f"music (not generated): {music}")

    speech_url = None
    try:
        speech_url = f"{base}/tracks/{speak(plan.say)}"
    except httpx.HTTPError as exc:
        # Losing the voice is survivable — the screen still says what happened.
        print(f"tts failed: {exc}")

    return {
        "say": plan.say,
        "screen": plan.screen[:20],
        "music": music,
        "speech_url": speech_url,
        # True means: play the speech, then GET /track for the music.
        "music_pending": generating,
        "audio_url": None,
        "volume": round(volume, 2),
    }


@app.post("/ambient")
def post_ambient(reading: Ambient):
    """Where the sensor board posts. Pydantic validates at the boundary, so a
    malformed payload gets a 422 instead of poisoning the next prompt."""
    global ambient, ambient_at
    ambient, ambient_at = reading, time.time()
    print(f"ambient: {describe_ambient()}  (raw: {reading.model_dump_json()})")

    # The node has a screen but no way to know the weather or where it is, so the
    # reply to its own POST carries that back. No extra request, no extra endpoint.
    sky = weather_now()
    here = locate()
    return {
        "ok": True,
        "place": here[2] if here else "",
        # Already split for a 160x80 panel — the device should not be parsing prose.
        "sky": sky.split(" and ")[0] if sky else "",
        "outside_c": _weather[2],
        "time": time.strftime("%H:%M"),
    }


@mcp.tool
def get_vibe() -> str:
    """The room right now: time of day, light level, temperature, humidity.

    Use it to choose music when the request is open-ended. Returns plain English,
    or just the time of day if no sensor board is reporting.
    """
    return describe_ambient()


@app.get("/track")
def track(request: Request):
    """Wait for the in-flight generation and hand back its URL.

    The device calls this once the spoken reply has finished playing. Blocking is
    deliberate — by then the track is usually already done, and a blocking read is
    far less firmware than a polling loop.
    """
    global pending
    if pending is None:
        return {"audio_url": None, "screen": "Nothing queued"}

    try:
        name = pending.result(timeout=GENERATION_TIMEOUT)
    except Exception as exc:
        print(f"generation failed: {exc}")
        return {"audio_url": None, "screen": "Failed - retry"}
    finally:
        pending = None

    return {"audio_url": f"{str(request.base_url).rstrip('/')}/tracks/{name}",
            "screen": "Now playing"}


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
