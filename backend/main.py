"""soundBud backend — voice in, generated music out.

    POST /utterance   raw 16kHz mono WAV  ->  {say, screen, audio_url, volume}
    GET  /tracks/*.mp3                        the generated audio

Pipeline: transcribe -> Claude decides what the user meant -> generate if needed.

Run:  uvicorn main:app --host 0.0.0.0 --port 8000
      (0.0.0.0, not 127.0.0.1 — the device is a different machine)
"""

import json
import os
import time
from urllib.parse import urlencode
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
from typing import Literal

import anthropic
import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastmcp import FastMCP
from pydantic import BaseModel

# Keys live in .env at the repo root (gitignored). Copy .env.example to start.
load_dotenv(Path(__file__).parent.parent / ".env")

ELEVEN = "https://api.elevenlabs.io/v1"

# Sonnet, not Haiku. Intent classification looked easy with four intents and
# stopped being easy at eight: Haiku scored 8/10 on the real phrasings people
# use ("could you skip this one", "play that again") against Sonnet's 10/10.
# It costs about 2s more, but that lands inside a wait the user already has —
# generation takes 12-15s — and a misread intent means the device silently does
# nothing, which is the failure that actually gets reported.
MODEL = os.getenv("SOUNDBUD_MODEL", "claude-sonnet-5")
TRACKS = Path(__file__).parent / "tracks"
TRACKS.mkdir(exist_ok=True)

# Music generation is slow. This must be generous or we give up on a request
# that was going to succeed.
GENERATION_TIMEOUT = 180.0

# How a remote agent reaches us for the mp3s. An MCP tool has no request to read
# a host off, unlike /utterance, so this has to be the LAN address — a localhost
# URL handed to another machine points that machine at itself and fails silently.
# Detected rather than configured, because forgetting to set it is the single
# easiest way to break the agent path.
def _lan_ip() -> str:
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))     # no packets sent; just picks the route
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


PUBLIC_URL = os.getenv("SOUNDBUD_URL", f"http://{_lan_ip()}:8000").rstrip("/")

# Set SOUNDBUD_AUDIO=0 to skip generation and return only the text description
# in `music`. Useful when testing against a device that has a screen but no
# speaker — saves a minute of waiting and the ElevenLabs credits.
GENERATE_AUDIO = os.getenv("SOUNDBUD_AUDIO", "1") == "1"

# 2 minutes. The device streams rather than buffers, so length costs generation
# time and ElevenLabs credits, not memory. Claude picks the duration and is not
# schema-constrained, so this is enforced, not requested.
MAX_DURATION_MS = 120_000
MIN_DURATION_MS = 30_000

# 8s of 16kHz mono PCM is ~256KB. 2MB leaves room for a longer clip without
# letting an unbounded body read chew all our memory.
MAX_UPLOAD_BYTES = 2_000_000
WAV_HEADER_BYTES = 44

# Stock ElevenLabs voice. flash_v2_5 because this sits directly in front of the
# user — quality matters less than getting a reply out while music generates.
VOICE_ID = os.getenv("SOUNDBUD_VOICE", "21m00Tcm4TlvDq8ikWAM")
TTS_MODEL = "eleven_flash_v2_5"

# ─── Spotify ────────────────────────────────────────────────────────────────
# Read-only use (what is playing) works on a free account. Starting playback
# needs Premium and an already-running Spotify client to target — there is no
# DRM-free stream, so Spotify audio can never come out of our own speaker.
SPOTIFY_ID = os.getenv("SPOTIFY_CLIENT_ID", "")
SPOTIFY_SECRET = os.getenv("SPOTIFY_CLIENT_SECRET", "")
# Spotify only permits plain http for the literal loopback address.
SPOTIFY_REDIRECT = os.getenv("SPOTIFY_REDIRECT_URI",
                             "http://127.0.0.1:8000/spotify/callback")
SPOTIFY_SCOPES = ("user-read-currently-playing user-read-playback-state "
                  "user-modify-playback-state")
SPOTIFY_API = "https://api.spotify.com/v1"
# Refresh token outlives the process; gitignored.
SPOTIFY_TOKEN_FILE = Path(__file__).parent / ".spotify_token.json"
_spotify_access: tuple[float, str] = (0.0, "")   # (expires_at, token)
# Title and artist names last read from Spotify, used to keep them out of
# generation prompts. See strip_names().
_last_heard: list[str] = []

# ─── track library ──────────────────────────────────────────────────────────
# Every generated track already lives in tracks/ — this is the index that makes
# them findable again, instead of orphaned files nobody can reach.
INDEX_FILE = TRACKS / "index.json"


def library() -> list[dict]:
    if not INDEX_FILE.exists():
        return []
    try:
        return json.loads(INDEX_FILE.read_text())
    except Exception as exc:      # a corrupt index must not break generation
        print(f"track index unreadable, starting fresh: {exc}")
        return []


def remember(file: str, prompt: str, genre: str, label: str) -> None:
    entries = library()
    entries.insert(0, {"file": file, "prompt": prompt, "genre": genre.lower(),
                       "label": label, "created": int(time.time()), "plays": 1})
    INDEX_FILE.write_text(json.dumps(entries[:200], indent=1))


# "the previous one" must not resolve to the track currently playing, or a
# replay sounds like a restart — which is exactly what it did.
BEFORE_THIS = ("previous", "before", "last one", "earlier", "one before",
               "another", "different", "other")


def find_in_library(want: str, exclude: str | None = None) -> dict | None:
    """Best existing track for a genre or description, or None.

    Generation costs 15 seconds and real money; if we already made something
    that fits, playing it back beats making a near-duplicate.
    """
    # Only offer tracks whose file is still on disk — the index outlives cleanups.
    entries = [e for e in library() if (TRACKS / e["file"]).exists()]
    want = want.lower().strip()

    # "play the previous one" means step back past whatever is playing now.
    stepping_back = any(w in want for w in BEFORE_THIS)
    if stepping_back and exclude:
        entries = [e for e in entries if e["file"] != exclude]
    if not entries:
        return None

    # Strip the positional words so "the previous lo-fi one" still searches for
    # lo-fi, and a bare "previous" is left meaning "newest of what remains".
    for word in BEFORE_THIS:
        want = want.replace(word, " ")
    want = " ".join(want.replace("one", " ").replace("song", " ")
                    .replace("track", " ").split()).strip()

    if not want:
        return entries[0]        # newest, or newest-but-current if stepping back

    for entry in entries:                        # newest first
        if want in entry["genre"] or want in entry["label"].lower():
            return entry
    words = {w for w in want.split() if len(w) > 3}
    for entry in entries:
        if words & set(entry["prompt"].lower().split()):
            return entry

    # Nothing matched the words, but if they asked for an earlier track then
    # "an earlier track" is itself the request — leftovers like "you made" or
    # "this" are noise, not search terms.
    return entries[0] if stepping_back else None

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
    genre: str = ""      # short tag for the library
    label: str = ""      # human name, mirrors Plan.screen


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
    intent: Literal["set_volume", "modify_track", "new_track", "transport",
                    "spotify_play", "answer", "replay", "keep_playing"]
    volume: float | None   # absolute target 0..1, only for set_volume
    track: TrackSpec | None  # only for modify_track / new_track
    spotify_query: str | None  # search terms, only for spotify_play
    genre: str | None          # short tag for the library, e.g. "lo-fi hip hop"
    replay_query: str | None   # what to look for in the library, only for replay
    keep_playing: bool | None  # only for keep_playing: True = on, False = off
    # only for transport: what to actually do
    transport_action: Literal["pause", "resume", "next", "previous"] | None
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
- new_track: a fresh request unrelated to what's playing. This GENERATES music.
- transport: playback control — "skip", "next", "stop", "pause", "resume",
  "go back", "previous". Set `transport_action` to pause, resume, next or
  previous. "stop" and "pause" both map to pause.

  IMPORTANT: transport controls SPOTIFY, which plays on the user's phone or
  laptop — not on this speaker. Only choose it when the context says something
  is on their Spotify right now. Getting this wrong is silent failure: Spotify
  obeys, the speaker stays quiet, and the user thinks nothing happened.

  When nothing is on their Spotify, playback words mean OUR tracks:
    "skip this" / "next"      -> replay with replay_query "another"
    "play the previous song"  -> replay with replay_query "previous"
  Never answer in these cases — they asked for a change and expect one.
- spotify_play: the user named a real, existing song or artist and wants THAT
  recording — "play Bohemian Rhapsody", "put on some Radiohead", "play Blue in
  Green by Bill Evans". Set `spotify_query` to what to search for. Do not use it
  for genres or moods; "play some jazz" is new_track, because they want music
  like that, not one specific recording.

  This also covers a song the user can only half-describe: quoted lyrics, a
  hummed hook they put into words, "that one by Dave about running". You know an
  enormous amount of popular music — work out what they mean and put the actual
  title and artist in `spotify_query`, not the lyrics. "the one that goes is this
  the real life" becomes "Bohemian Rhapsody Queen". If you genuinely cannot place
  it, put the best search terms you can and say in `say` that you are guessing.

- replay: the user wants a track WE generated earlier — "play that again", "the
  lo-fi one from earlier", "the previous song you made", "put that jazz track
  back on". Set `replay_query` to what to look for: a genre, a mood, "previous"
  for the one before what is playing, or "" for the most recent. Nothing is
  generated, so this is instant and free.

  Keep the user's own wording in `replay_query` when they say "previous",
  "earlier" or "the one before" — that is what steps back past the current
  track instead of restarting it.
- keep_playing: "keep playing", "keep it going", "don't stop", "autoplay",
  "continue the same vibe" turns it ON (`keep_playing` true); "stop after this",
  "just this one" turns it OFF. When on, a new track is made automatically as
  each one ends.
  ALSO fill in `track` when turning it on, exactly as you would for new_track —
  turning autoplay on starts music immediately. If something is already playing,
  base it on that. If nothing is, pick something that suits.
- answer: a QUESTION that needs no action — "what is this song?", "who sings
  this?", "what's playing?", "what can you do?". Put the reply in `say` and
  change nothing. A question is never a request to play something: "what is this
  song" must not restart it.

  Only when the user wants INFORMATION and nothing else. Politeness is not a
  question — this is a voice device, people phrase requests as questions all the
  time and they still expect action:

    "can you play the previous song?"  -> replay, NOT answer
    "could you skip this?"             -> transport, NOT answer
    "would you turn it down?"          -> set_volume, NOT answer
    "what is this song?"               -> answer (they want to be told)

  Test it this way: if doing nothing but talking would disappoint them, it is
  not answer. Choose answer only when there is genuinely nothing to do.

Deciding between new_track and spotify_play is the second most important call you
make. A named song or artist is spotify_play. A description is new_track. A
question about either is answer.

If genuinely ambiguous between set_volume and modify_track, prefer set_volume:
it is instant, free, and trivially corrected.

Build the track from what the user actually said, and nothing else. "Play
something" means pick something good — invent a genre, do not hedge.

NEVER put a song title, artist name, album or band into `track.prompt`. Two
reasons, and both matter:

- The generator cannot reproduce a specific recording and will refuse prompts
  that ask it to. A named track is a rejected request, not a better one.
- A name carries no musical information. "Jamaican (Bam Bam) by HUGEL" tells it
  nothing; "latin house around 126bpm, dancehall vocal chops, tropical
  percussion, filtered piano stabs, warm analogue bass" tells it everything.

So when the user asks for something like what they are hearing, translate the
track into its musical qualities and describe those. You know how these artists
and records sound — write down what you know:

  genre and subgenre, tempo, key feel (major/minor, bright/dark),
  instrumentation, rhythm and groove, production style and era,
  vocal character if any, energy and mood

The goal is a track in that vein — a new piece a listener would file next to it —
not a copy of it. Aim for the style, not the song.

You may name the track in `say` and `screen`, since those are only spoken and
displayed. Just never in `track.prompt`.

Sometimes you are given a line starting "Right now it is:" with the time, the
room's light and temperature, the location and the outside weather. It only
appears when the user asked for it — "for the current vibe", "match the mood",
"something that fits the weather". When it is there, use it: weather first, since
rain and fog want something different from clear sun, then light and temperature,
and let the location colour the genre — flamenco in Spain, garage or trip hop in
London. When it is absent, do not speculate about the room or the season.

Even when you have it, never read the readings aloud in `say`. "Something warm
for the evening" is good; "it is 19°C and dim so here is jazz" is not — nobody
wants their thermostat narrating.

On every new_track and modify_track, also set `track.genre` to a SHORT tag —
"lo-fi hip hop", "drum and bass", "ambient", "flamenco" — two or three words, no
punctuation. It is what makes the track findable later, and what stops us
generating a near-duplicate of something we already have. Set `track.label` to
the same text as `screen`.

`duration_ms` must be 120000.

`screen` must be at most 20 characters — it goes on a 160x80 display. Name the
MUSIC, never the state: the device already shows whether it is playing, so
"Now Playing" and "Loading" waste the only line you get. "Dreamy Lo-Fi",
"Rainy Day Jazz", "Tomorrowland" all tell the user something. Title Case, no
trailing punctuation. For volume and transport, describing the action is fine —
"Volume 70%", "Skipped".

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
# Claude's short label for what is playing. Kept so /track can hand back the same
# title the reply used, instead of a generic one the screen learns nothing from.
now_playing: str = ""

# Music generation runs here while the spoken reply goes back to the device, so
# the user hears something within a couple of seconds instead of waiting out the
# whole generation in silence. One worker: one device, one track at a time.
_pool = ThreadPoolExecutor(max_workers=1)
pending: Future | None = None
# When on, a fresh track is queued the moment one is handed over, so the next is
# already generating while the current plays and the room never goes quiet.
keep_playing: bool = False
# A library track ready to hand over immediately. Replay reuses the normal
# two-step flow — reply, then GET /track — rather than inventing a second
# channel the firmware would have to learn.
ready_track: str | None = None
# The file the device was last given. "Previous" means the one before this.
current_file: str | None = None

# ─── playback bridge ────────────────────────────────────────────────────────
# The remote has the microphone, the base has the speaker, and they never talk
# to each other. So the base polls here: `seq` changes whenever there is
# something new to do, which is all the firmware needs to compare against.
playback: dict = {"seq": 0, "speech_url": None, "audio_url": None,
                  "screen": "", "volume": 0.6, "keep_playing": False,
                  "action": "", "music_pending": False,
                  # For the remote's display — it has a screen but no speaker.
                  "now_playing": "", "state": "idle"}


def queue_playback(**fields) -> None:
    playback.update(fields)
    playback["seq"] += 1
    print(f"playback #{playback['seq']}: {fields.get('action') or 'play'} "
          f"{(fields.get('speech_url') or fields.get('audio_url') or '').split('/')[-1]}")

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


# Telling the model to ignore the readings does not work — it uses whatever is in
# front of it, and "cool London evening" leaks into tracks nobody asked that of.
# So the readings are withheld unless the request reaches for them.
VIBE_WORDS = (
    "vibe", "mood", "atmosphere", "ambience", "ambiance", "weather", "room",
    "outside", "rain", "sun", "cloud", "fog", "snow", "storm", "temperature",
    "in here", "right now", "fits", "match", "read the room", "feel",
)


def wants_vibe(utterance: str) -> bool:
    low = utterance.lower()
    return any(w in low for w in VIBE_WORDS)


def plan_from(utterance: str) -> Plan:
    context = (
        f"Currently playing: {current_track.model_dump_json()}"
        if current_track
        else "Nothing is playing yet."
    )
    # "make something like this" needs to know what "this" is. Fetched only when
    # referenced, same reasoning as the ambient readings.
    if any(w in utterance.lower() for w in
           ("this song", "this track", "playing", "spotify", "like this",
            "what is this", "what's this", "same style", "similar")):
        try:
            heard = spotify_now_playing()
        except Exception as exc:      # never let Spotify break a voice request
            print(f"spotify lookup failed, continuing without it: {exc}")
            heard = ""
        if heard:
            context += f"\nOn the user's Spotify right now: {heard}"
            print(f"spotify: {heard}")

    if wants_vibe(utterance):
        vibe = describe_ambient()
        context += f"\nRight now it is: {vibe}"
        print(f"vibe (asked for): {vibe}")
    else:
        print("vibe: withheld — request did not ask for it")
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
    plan = response.parsed_output
    if plan is None:
        # The model declined to produce a Plan — refusal, or a stop reason that
        # left nothing parsed. A 500 here strands the device mid-conversation,
        # so fall back to something it can act on.
        print(f"planner returned nothing for {utterance!r}; treating as answer")
        return Plan(intent="answer", volume=None, track=None, spotify_query=None,
                    transport_action=None, genre=None, replay_query=None,
                    keep_playing=None,
                    say="Sorry, I did not catch that. Say it again?",
                    screen="Say again?")
    return plan


def strip_names(prompt: str) -> str:
    """Remove the currently-playing title and artist from a generation prompt.

    A backstop for the instruction in SYSTEM. Naming a record asks the generator
    to reproduce it, which it refuses — and the name was never carrying musical
    information anyway. What survives is the description, which is the part that
    actually shapes the output.
    """
    heard = _last_heard
    if not heard:
        return prompt
    out = prompt
    for name in heard:
        if len(name) < 4:          # too short to match safely
            continue
        idx = out.lower().find(name.lower())
        if idx >= 0:
            out = (out[:idx] + out[idx + len(name):])
            print(f"stripped {name!r} from the generation prompt")
    # Tidy the punctuation the removal leaves behind.
    out = " ".join(out.replace(" ,", ",").split())
    return out.strip(" ,-—") or prompt


def generate(spec: TrackSpec) -> str:
    """Generate a track, save it, return its filename."""
    started = time.monotonic()
    length_ms = min(MAX_DURATION_MS, max(MIN_DURATION_MS, spec.duration_ms))
    prompt = strip_names(spec.prompt)
    r = httpx.post(
        f"{ELEVEN}/music",
        headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"]},
        json={
            "prompt": prompt,
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
    remember(name, prompt, spec.genre or "", spec.label or "")
    print(f"generated {name} in {time.monotonic() - started:.1f}s "
          f"({len(r.content) // 1024}KB) :: {spec.prompt}")
    return name


def spotify_token() -> str:
    """A valid access token, refreshed on demand. "" if not connected yet."""
    global _spotify_access
    expires, token = _spotify_access
    if token and time.time() < expires:
        return token
    if not (SPOTIFY_ID and SPOTIFY_SECRET and SPOTIFY_TOKEN_FILE.exists()):
        return ""
    refresh = json.loads(SPOTIFY_TOKEN_FILE.read_text())["refresh_token"]
    r = httpx.post("https://accounts.spotify.com/api/token",
                   data={"grant_type": "refresh_token", "refresh_token": refresh},
                   auth=(SPOTIFY_ID, SPOTIFY_SECRET), timeout=15.0)
    r.raise_for_status()
    body = r.json()
    # Refresh tokens are usually reused, but Spotify may rotate them.
    if body.get("refresh_token"):
        SPOTIFY_TOKEN_FILE.write_text(json.dumps({"refresh_token": body["refresh_token"]}))
    _spotify_access = (time.time() + body["expires_in"] - 60, body["access_token"])
    return _spotify_access[1]


def spotify_get(path: str, **params):
    """GET from the Spotify API, returning None rather than raising.

    Everything read from Spotify is enrichment — nice to have, never required to
    answer the user. A 204 means nothing is playing; 403 means the endpoint is
    restricted for new apps (Spotify cut several off in Nov 2024). Neither should
    turn into a 500 on a voice request.
    """
    token = spotify_token()
    if not token:
        return None
    try:
        r = httpx.get(f"{SPOTIFY_API}{path}", params=params,
                      headers={"Authorization": f"Bearer {token}"}, timeout=15.0)
    except httpx.HTTPError as exc:
        print(f"spotify {path} unreachable: {exc}")
        return None
    if r.status_code >= 400:
        print(f"spotify {path} -> {r.status_code} (ignored)")
        return None
    return r.json() if r.content else None


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
def generate_music(prompt: str, duration_seconds: int = 120,
                   instrumental: bool = True) -> str:
    """Generate a music track and return a URL to the mp3.

    `prompt` describes the music: genre, mood, instruments, tempo. Be specific —
    "slow lo-fi hip hop, warm rhodes, vinyl crackle" beats "chill music".
    Takes up to a minute or two. Duration is clamped to 30..120 seconds.
    """
    global current_track
    spec = TrackSpec(prompt=prompt, duration_ms=duration_seconds * 1000,
                     instrumental=instrumental)
    url = f"{PUBLIC_URL}/tracks/{generate(spec)}"
    # Remembered so a follow-up like "make it calmer" has something to adjust.
    current_track = spec
    return url


@app.get("/playback")
def playback_state(since: int = -1):
    """What the base should be doing. Polled once a second by the speaker board.

    `seq` increases whenever there is something new. The device keeps the last
    seq it acted on and ignores anything not greater — so a missed poll costs
    nothing and a repeated one does not replay the same track.

    Long-polls for up to 25s when `since` matches, so a quiet system is one open
    connection rather than a request every second.
    """
    deadline = time.time() + 25.0
    while playback["seq"] <= since and time.time() < deadline:
        time.sleep(0.25)
    return playback


@app.get("/history")
def history(limit: int = 30):
    """Everything generated so far, newest first."""
    return {"tracks": [{**e, "url": f"{PUBLIC_URL}/tracks/{e['file']}"}
                       for e in library()[:limit]]}


@mcp.tool
def list_library(limit: int = 20) -> str:
    """Tracks already generated, newest first, with their genres.

    Check here before generating: if something close already exists, replaying it
    is instant and free, while generating a near-duplicate costs 15 seconds and
    real money. Use play_from_library to play one.
    """
    entries = library()[:limit]
    if not entries:
        return "Nothing generated yet."
    return "\n".join(
        f"{i}. {e['label'] or e['genre'] or 'untitled'} [{e['genre']}] — {e['prompt'][:70]}"
        for i, e in enumerate(entries))


@mcp.tool
def play_from_library(query: str = "") -> str:
    """URL of an already-generated track matching a genre or description.

    Empty query returns the most recent. Returns "" if nothing matches, in which
    case generate something instead.
    """
    hit = find_in_library(query) or (library()[0] if not query and library() else None)
    if not hit:
        return ""
    return f"{PUBLIC_URL}/tracks/{hit['file']}"


@mcp.tool
def set_keep_playing(on: bool) -> str:
    """Turn continuous play on or off.

    When on, a new track is generated as each one is handed over, so playback
    never stops. The device keeps asking for the next track by itself.
    """
    global keep_playing
    keep_playing = on
    return f"Keep playing is {'on' if on else 'off'}."


@mcp.tool
def get_now_playing() -> str:
    """The prompt behind the track currently playing, or "" if nothing is.

    Call this before a follow-up request like "make it calmer" or "add drums":
    take this prompt, change only what the user asked for, and pass the whole
    edited prompt to generate_music. Editing beats starting over — the user
    expects the same piece adjusted, not a different one.
    """
    return current_track.prompt if current_track else ""


def apply(plan: Plan, track: TrackSpec | None, vol: float):
    """Fold a Plan into session state. Pure — no network, no globals.

    Returns (track, volume, needs_generation).
    """
    if plan.intent == "set_volume":
        # Claude is asked for an absolute target but is not schema-constrained
        # to 0..1, so clamp rather than trust.
        vol = min(1.0, max(0.0, plan.volume if plan.volume is not None else vol))
        return track, vol, False

    if plan.intent in ("transport", "spotify_play"):
        return track, vol, False   # Spotify plays on the user's own device

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
async def utterance(request: Request, volume_now: float | None = None):
    """`volume_now` is the device's actual 0..1 level, which the knob can change
    without telling us. Trust it over our own copy, or "turn it down" is computed
    from a stale number."""
    global current_track, volume, now_playing, ready_track, keep_playing, pending, current_file
    if volume_now is not None:
        volume = min(1.0, max(0.0, volume_now))

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

    # base_url is whatever host the device reached us on, so URLs are already the
    # right LAN address without configuring it anywhere.
    base = str(request.base_url).rstrip("/")

    # Every intent below may change what we say, and whether there is anything
    # for the device to collect afterwards. Decide all of that first, then start
    # the generation, so nothing is queued before we know it is wanted.
    spoken = plan.say
    screen = plan.screen[:20]
    generating = False
    stop_action = ""          # "pause"/"resume" for our own speaker

    # Replay is instant, but it still goes out through /track so the device can
    # use the flow it already knows: speak the reply, then come and collect.
    if plan.intent == "replay":
        hit = find_in_library(plan.replay_query or "", exclude=current_file)
        if hit:
            ready_track = hit["file"]
            now_playing = (hit["label"] or hit["genre"] or "Replay")[:20]
            screen = now_playing
            generating = True          # tells the device to fetch /track
            print(f"replay: {hit['file']} ({hit['genre'] or 'no genre'})")
        else:
            spoken = "I could not find that one. Want me to make something new?"

    if plan.intent == "keep_playing" and plan.keep_playing is not None:
        keep_playing = plan.keep_playing
        print(f"keep playing: {'on' if keep_playing else 'off'}")
        # Turning it on has to start something, or it is a switch that appears
        # to do nothing: the follow-up is only queued when a track is handed
        # over, and none ever is until one plays.
        if keep_playing:
            seed = plan.track or current_track
            if seed and GENERATE_AUDIO:
                current_track = seed
                now_playing = (plan.screen or seed.label or "Playing")[:20]
                pending = _pool.submit(lambda: generate(seed))
                generating = True
                print(f"keep playing: first track queued :: {seed.prompt[:60]}")
            else:
                spoken = ("Autoplay is on. Ask for something and I will keep it "
                          "going from there.")

    if plan.intent == "transport" and plan.transport_action:
        result = spotify_control(plan.transport_action)
        print(f"transport {plan.transport_action} -> {result}")
        # Whatever Spotify did, a pause also has to reach our own speaker.
        if plan.transport_action in ("pause", "resume"):
            stop_action = plan.transport_action
        # Only speak Spotify's answer when it actually did something; otherwise
        # keep Claude's reply, since the device controls its own playback.
        if result.startswith(("Spotify is not", "No active")):
            # Do not report success the user cannot hear.
            spoken = ("Nothing is playing on Spotify. Say \"play the previous "
                      "song you made\" if you meant one of mine.")
        else:
            spoken = f"{result} On your Spotify."
    if plan.intent == "spotify_play" and plan.spotify_query:
        spoken = spotify_play(plan.spotify_query)
        print(f"spotify_play({plan.spotify_query!r}) -> {spoken}")

    # What the track would be, in words. Useful on the screen while there is no
    # speaker, and useful for debugging once there is one.
    music = current_track.prompt if needs_generation else None

    # Now start the music, so it generates while the reply is being voiced.
    # A new request always supersedes an unfinished one: the device may never
    # have collected the last track, and refusing to generate because something
    # is still pending wedges the whole thing until a restart.
    if needs_generation and GENERATE_AUDIO:
        if pending is not None and not pending.done():
            pending.cancel()      # no-op if it already started; it just goes unused
            print("superseding the previous, uncollected generation")
        track = current_track
        now_playing = screen
        pending = _pool.submit(lambda: generate(track))
        generating = True
    elif music:
        print(f"music (not generated): {music}")

    speech_url = None
    try:
        speech_url = f"{base}/tracks/{speak(spoken)}"
    except httpx.HTTPError as exc:
        # Losing the voice is survivable — the screen still says what happened.
        print(f"tts failed: {exc}")

    # Publish for the base to collect. The remote holds the microphone and has
    # no speaker, so whatever it hears has to reach the other board through here.
    # Any new utterance supersedes whatever is playing. Without this the base
    # keeps the old track running underneath the new one, which is exactly what
    # "play another song" is asking it not to do.
    queue_playback(speech_url=speech_url, audio_url=None, screen=screen,
                   volume=round(volume, 2), keep_playing=keep_playing,
                   action=stop_action or ("speak" if speech_url else "stop"),
                   music_pending=generating,
                   now_playing=now_playing or "",
                   state="speaking" if speech_url else "idle")

    return {
        "say": spoken,
        "screen": screen,
        "music": music,
        "speech_url": speech_url,
        "keep_playing": keep_playing,
        # True means: play the speech, then GET /track for the music.
        "music_pending": generating,
        "audio_url": None,
        "volume": round(volume, 2),
        # Only honour `volume` when this is true. Otherwise it is just our copy
        # of what the device already has, and applying it would stamp on the knob.
        "set_volume": plan.intent == "set_volume",
    }


@app.get("/spotify/login")
def spotify_login():
    """Open this in a browser once to connect an account. Redirects to Spotify,
    which sends the user back to /spotify/callback with a code."""
    if not SPOTIFY_ID:
        raise HTTPException(500, "SPOTIFY_CLIENT_ID not set in .env")
    q = urlencode({"client_id": SPOTIFY_ID, "response_type": "code",
                   "redirect_uri": SPOTIFY_REDIRECT, "scope": SPOTIFY_SCOPES})
    return RedirectResponse(f"https://accounts.spotify.com/authorize?{q}")


@app.get("/spotify/callback")
def spotify_callback(code: str = "", error: str = ""):
    if error:
        raise HTTPException(400, f"Spotify refused: {error}")
    r = httpx.post("https://accounts.spotify.com/api/token",
                   data={"grant_type": "authorization_code", "code": code,
                         "redirect_uri": SPOTIFY_REDIRECT},
                   auth=(SPOTIFY_ID, SPOTIFY_SECRET), timeout=15.0)
    r.raise_for_status()
    # Only the refresh token is worth keeping; access tokens last an hour.
    SPOTIFY_TOKEN_FILE.write_text(json.dumps({"refresh_token": r.json()["refresh_token"]}))
    print(f"spotify: connected, refresh token saved to {SPOTIFY_TOKEN_FILE.name}")
    return {"ok": True, "message": "Spotify connected. You can close this tab."}


@mcp.tool
def spotify_now_playing() -> str:
    """What is playing on the user's Spotify right now, with the artist's genres.

    Returns something like:
        "Bohemian Rhapsody by Queen (album: A Night at the Opera;
         genres: glam rock, classic rock)"

    Use it for "what's this?", and as the seed for "make me something like this"
    — you already know how these artists sound, so write an ElevenLabs prompt
    describing that style rather than quoting the genres back.

    Returns "" if nothing is playing or Spotify is not connected.
    """
    now = spotify_get("/me/player/currently-playing")
    item = (now or {}).get("item")
    if not item:
        return ""
    global _last_heard
    artists = [a["name"] for a in item.get("artists", [])]
    _last_heard = [item["name"], *artists]
    bits = f"{item['name']} by {', '.join(artists) or 'unknown'}"
    if item.get("album", {}).get("name"):
        bits += f" (album: {item['album']['name']}"
        # Genres live on the artist, not the track. /audio-features is dead since
        # Nov 2024, so this is the only style signal the API still gives us.
        ids = [a["id"] for a in item.get("artists", []) if a.get("id")]
        genres: list[str] = []
        if ids:
            info = spotify_get("/artists", ids=",".join(ids[:5])) or {}
            for a in info.get("artists", []):
                genres += a.get("genres", [])
        if genres:
            unique = list(dict.fromkeys(genres))[:6]
            bits += f"; genres: {', '.join(unique)}"
        bits += ")"
    return bits


@mcp.tool
def spotify_control(action: str) -> str:
    """Control Spotify playback: "pause", "resume", "next" or "previous".

    Acts on the user's own Spotify device. Needs Premium.
    """
    token = spotify_token()
    if not token:
        return "Spotify is not connected."

    verb, path = {
        "pause":    ("PUT",  "/me/player/pause"),
        "stop":     ("PUT",  "/me/player/pause"),
        "resume":   ("PUT",  "/me/player/play"),
        "play":     ("PUT",  "/me/player/play"),
        "next":     ("POST", "/me/player/next"),
        "skip":     ("POST", "/me/player/next"),
        "previous": ("POST", "/me/player/previous"),
    }.get(action.lower(), (None, None))
    if verb is None:
        return f"Cannot do {action!r} on Spotify."

    r = httpx.request(verb, f"{SPOTIFY_API}{path}",
                      headers={"Authorization": f"Bearer {token}"}, timeout=15.0)
    if r.status_code == 404:
        return "No active Spotify device to control."
    if r.status_code == 403:
        # Also returned when the requested state already holds, e.g. pausing
        # something that is already paused.
        return "Spotify would not accept that — it may already be in that state."
    if r.status_code >= 400:
        print(f"spotify {path} -> {r.status_code}")
        return "Spotify did not accept that."
    return {"pause": "Paused.", "stop": "Paused.", "resume": "Playing again.",
            "play": "Playing again.", "next": "Skipped.", "skip": "Skipped.",
            "previous": "Went back."}.get(action.lower(), "Done.")


@mcp.tool
def spotify_play(query: str) -> str:
    """Search Spotify and start playing the best match. Needs Premium.

    IMPORTANT: this plays on the user's own Spotify device — their phone or
    laptop — not on this speaker. Spotify has no stream we can play. Say so if
    nothing comes out of the speaker.
    """
    token = spotify_token()
    if not token:
        return "Spotify is not connected. Open /spotify/login in a browser first."

    found = spotify_get("/search", q=query, type="track", limit=1) or {}
    items = found.get("tracks", {}).get("items", [])
    if not items:
        return f"Nothing on Spotify matched {query!r}."
    track = items[0]
    label = f"{track['name']} by {', '.join(a['name'] for a in track['artists'])}"

    r = httpx.put(f"{SPOTIFY_API}/me/player/play",
                  json={"uris": [track["uri"]]},
                  headers={"Authorization": f"Bearer {token}"}, timeout=15.0)
    if r.status_code == 404:
        return (f"Found {label}, but no active Spotify device. Open Spotify on a "
                "phone or laptop and press play once, then ask again.")
    if r.status_code == 403:
        return f"Found {label}, but playback control needs Spotify Premium."
    r.raise_for_status()
    return f"Playing {label} on your Spotify device."


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
    global pending, ready_track, keep_playing, current_file
    if ready_track:
        name, ready_track = ready_track, None
        current_file = name
        print(f"serving from library: {name}")
        url = f"{str(request.base_url).rstrip('/')}/tracks/{name}"
        queue_playback(speech_url=None, audio_url=url,
                       screen=now_playing or "Playing", volume=round(volume, 2),
                       keep_playing=keep_playing, action="play",
                       music_pending=False, now_playing=now_playing or "",
                       state="playing")
        return {"audio_url": url, "screen": now_playing or "Playing",
                "keep_playing": keep_playing}
    if pending is None:
        return {"audio_url": None, "screen": "Nothing queued"}

    try:
        name = pending.result(timeout=GENERATION_TIMEOUT)
    except Exception as exc:
        print(f"generation failed: {exc}")
        return {"audio_url": None, "screen": "Failed - retry"}
    finally:
        pending = None
    current_file = name

    # Queue the follow-up now, so it generates while this one plays. By the time
    # the device asks again it is usually already waiting.
    if keep_playing and current_track is not None:
        nxt = current_track
        pending = _pool.submit(lambda: generate(nxt))
        print("keep playing: next track queued")

    return {"audio_url": f"{str(request.base_url).rstrip('/')}/tracks/{name}",
            "screen": now_playing or "Playing",
            # Device: when this finishes, come back to /track instead of idling.
            "keep_playing": keep_playing}


if __name__ == "__main__":
    # Smallest check that fails if the state logic breaks. No network needed.
    chill = TrackSpec(prompt="chill lo-fi", duration_ms=30000, instrumental=True)

    t, v, gen = apply(
        Plan(intent="new_track", volume=None, track=chill, spotify_query=None, transport_action=None, genre=None, replay_query=None, keep_playing=None, say="", screen=""),
        None, 0.6)
    assert gen and t == chill

    warmer = chill.model_copy(update={"prompt": "chill lo-fi, warmer"})
    t2, _, gen2 = apply(
        Plan(intent="modify_track", volume=None, track=warmer, spotify_query=None, transport_action=None, genre=None, replay_query=None, keep_playing=None, say="", screen=""),
        t, v)
    assert gen2 and t2.prompt.endswith("warmer")

    # volume never generates, and is clamped
    _, v3, gen3 = apply(
        Plan(intent="set_volume", volume=1.9, track=None, spotify_query=None, transport_action=None, genre=None, replay_query=None, keep_playing=None, say="", screen=""),
        t2, v)
    assert not gen3 and v3 == 1.0

    print("ok")
