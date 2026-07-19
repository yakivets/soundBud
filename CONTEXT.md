# soundBud — AI-native music station

You talk to it, it generates music, it plays on its own speaker. Follow-up commands modify
the track you're hearing instead of starting over. Built at Axiometa London hackathon.

**Status: working end to end.** Hold the button, speak, and a generated track plays from the
device's own speaker. Backend in `backend/`, firmware in `firmware/soundbud/`, enclosure
plates in `cad/`.

**Scope: the prototype.** One device — voice in, generated song out of its own speaker,
follow-up commands that modify what's playing. The earlier plan for a separate handheld
remote is dropped: one Genesis-class board carries mic, amp, screen and button, so there is
no second firmware, no pairing, and no device-to-device protocol.

### What the pieces do

| Path | What it is |
|---|---|
| `backend/main.py` | FastAPI. `POST /utterance` takes a raw WAV and returns a spoken reply; `GET /track` hands over the music once it is ready. |
| `backend/talk.py` | Laptop stand-in for the device — same contract, useful when the hardware is not on the bench. |
| `firmware/remote/` | Handheld: button, mic, buzzer, screen. Records and POSTs. |
| `firmware/base/` | Speaker: amp, matrix, encoder, DHT11. Polls for work. |
| `cad/plates.py` | CadQuery script deriving sandwich-mount plates from the vendor STEP. |

### The reply comes back before the music

Generation takes 10-15 seconds, which is a long silence to sit through. So the
request is split in two:

```
POST /utterance ──► transcribe, ask Claude, START generating in background
                    voice Claude's reply with TTS  (~2s)
                └─► {say, screen, speech_url, music_pending: true}   ~5s total

        device plays the spoken reply while the track finishes generating

GET /track ─────► blocks on the in-flight generation, usually already done
                └─► {audio_url}
```

The user hears an answer at ~5s instead of nothing until ~15s. `say` was always
meant to cover generation latency; returning it after generation defeated that.

### Vibe is context, not a mode

A second Genesis Mini carries DHT11, LDR and GPS, and POSTs to `/ambient` every
60s. It never talks to the speaker — each board knows only the backend URL, so
there is no pairing and either can be off without affecting the other.

There is no "vibe intent". The readings are rendered to plain English ("evening,
dark, 13°C, cold, humid") and appended to the context Claude receives — but only
when the request reaches for them:

- "play something", "play some jazz", "surprise me" — readings withheld entirely.
- "for the current vibe", "match the mood", "fits the weather" — readings sent.

Withheld, not merely discouraged. Telling the model to ignore context in front of
it does not work: with the readings present, "play something" reliably came back
as "moody trip hop for a cool London evening" no matter what the prompt said. A
keyword check on the transcript (`wants_vibe`) decides, so the model cannot use
what it never sees.

Words rather than raw ADC because the model picks better music from "dim and
cold" than from a number it has to interpret. Readings older than 10 minutes are
dropped: a stale temperature is a lie about now.

`LIGHT_INVERTED` exists because an LDR divider can be wired either way — flip it
in software rather than rewiring.

### Continuous input stays on the device

Volume from the encoder calls `audio.setVolume()` directly and never touches the
backend. A knob has to answer instantly; routing it through `/utterance` would put
five seconds on every turn. Voice is the slow, semantic path — it picks the song.
The knob is the fast, continuous one. Keep them apart.

Consequence: the backend's `volume` and the device's drift apart once the knob is
touched, so "turn it down" is computed from a stale value. Fix when it matters by
passing the device's volume up with the next utterance.

### The Hermes agent owns the device-facing API

Decided 2026-07-18. The device points at the agent (`10.50.33.41:8000`), which
speaks the same contract this backend does — `GET /`, `POST /utterance?volume_now=`,
`GET /track`. This backend becomes the agent's tool provider, track host, and
ambient sink:

```
device ──► agent (Hermes) ──MCP──► this backend ──► ElevenLabs
sensor node ──────────────────────► /ambient
```

MCP tools at `/mcp/`: `generate_music`, `get_now_playing`, `get_vibe`.

`plan_from()`, `SYSTEM`, `apply()` and this backend's own `/utterance` are kept
but no longer on the device path. They still work — `talk.py` exercises them — and
are the fallback if the agent is unavailable.

`PUBLIC_URL` is auto-detected from the LAN route rather than defaulting to
localhost: an MCP tool has no request to read a host off, and handing another
machine a localhost URL fails in a way that looks like the agent's fault.

### Firmware lives in Axiometa Studio

Studio is the source of truth for both sketches; the copies here are version
history, updated when something works. The backend is the opposite — it runs from
this repo, so edit it here directly.

### Three things that cost hours

- **`audio_eof_mp3` never fires for `connecttohost`.** That callback is for
  `connecttoFS`; network streams use `audio_eof_stream`, and which exists varies
  by library version. End of stream is detected by polling `audio.isRunning()`
  with a 1.5s grace period instead. `isPaused` must be excluded from that check,
  or pausing reads as the track ending.
- **NeoPixel writes disable interrupts.** Calling `strip.show()` every loop
  audibly glitches the I2S stream. Both the matrix and the equaliser only push
  when their content actually changed.
- **The GPS is at 115200, not the datasheet's 9600.** Garbage in the raw echo
  means the wrong baud; silence means no bytes at all. Those look identical
  without an echo to read.

### Two facts worth not rediscovering

- **ElevenLabs rejects `seed` when `prompt` is set.** There is no continuity knob, so
  "make it calmer" re-rolls a new song rather than adjusting the current one.
- **Mounting holes are ⌀3.40mm (M3), not the ⌀2.7mm the product page claims.** Measured off
  the official STEP.

---

## Hardware

From the Axiometa catalogue (checked against the live product pages):

Two boards, both talking only to the backend — never to each other.

**Remote** (Genesis Mini) — `firmware/remote/`

| Port | Module |
|---|---|
| P1 | Button — hold to talk |
| P2 | Passive buzzer |
| P3 | IPS LCD 0.96" — status and replies |
| P4 | PDM mic |

**Base** (Genesis One) — `firmware/base/`

| Port | Module |
|---|---|
| P1 | NeoPixel 5×5 — volume digit / pause glyph |
| P2 | DHT11 |
| P4 | Rotary encoder — volume, push to pause |
| P5 | IPS LCD 0.96" — equaliser while playing |
| P6 | Passive buzzer |
| P7 | MAX98357A amp → speaker |

The remote hears, the base plays, and they never talk to each other. `/playback`
bridges them: the base long-polls it and acts on any `seq` above the last it
handled. `age_s` guards against a freshly booted board replaying a command from
before it existed.

**Keep the matrix off P8.** That is GPIO44 / UART0 — serial output gets clocked
into the LEDs as pixel data, and the symptom is half the matrix flickering with
digits rendered as scatter.

| Need | Part | Note |
|---|---|---|
| Brain | **Genesis Mini** (ESP32-S3-Mini-1-N4R2) | 4MB flash, 2MB PSRAM, WiFi. Modules plug into **AX22 ports** — no wiring. |
| Screen | **IPS LCD 0.96"** | ~160×80. Design for four words, not a dashboard. |
| Button | **LED Button** | Push-to-talk. Its LED doubles as the recording indicator, free. |
| Mic | **Digital I2S mic** | Not the kit's analog mic — ADC audio is too noisy for speech-to-text in a loud room. |
| Amp | **Audio Amplifier (MAX98357A)** | I2S digital amp. |
| Speaker | Passive 4–8Ω | |
| Hand detection | **Distance Sensor (ToF VL53L0X)** | There is no gesture sensor in the catalogue — see below. |

Also in the starter kit if useful: rotary encoder, 5×5 NeoPixel matrix, passive buzzer,
light sensor, DHT11, IR transceiver. Board also has a STEMMA QT (I2C) connector.

### There is no gesture sensor

No APDS-9960, no PAJ7620 — nothing that does swipe detection in hardware. Build it from the
**ToF distance sensor**, which returns hand-distance at ~50Hz:

- **Hover height → volume.** 5cm = quiet, 25cm = loud. Continuous, trivial, looks like magic.
- **Quick in-out chop → next track.** Distance drops below ~8cm and returns within ~400ms.
- Two ToF sensors side by side gives real left/right swipe. Only after one sensor works.

---

## Prototype architecture

One device. Laptop is the brain, ESP32 is the body.

```
   ESP32-S3                                    laptop (Python)
   ─────────                                   ───────────────
   button held  ──► record I2S mic
                    to PSRAM buffer
   button up    ──► send WAV blob ───────────► speech-to-text
                                                     │
                                               Claude: utterance + session state
                                                     │           → TrackSpec
                                                     ▼
                                               ElevenLabs /v1/music
                                                     │
                    play MP3 via I2S  ◄────────  serve MP3
                    → MAX98357A → speaker
   ToF distance ──► volume + next ───────────► player control
   screen       ◄── state + prompt text ──────
```

**Push-to-talk, not wake word.** Hold the touchpad, speak, release. Record into PSRAM, then
send the whole WAV as one blob. No audio streaming, no voice-activity detection, no wake-word
model — and it never misfires while someone is talking over you on stage. The PIXIE M1 has
**2MB PSRAM and 4MB flash** (N4R2); 10 seconds of 16kHz mono 16-bit is 320KB, so push-to-talk
fits comfortably — but the headroom is modest, so don't plan on buffering audio *and* an MP3
in RAM at the same time.

**Audio playback on the device.** ElevenLabs returns MP3. Use `ESP32-audioI2S`
(schreibfaul1) — its `connecttohost()` streams an MP3 straight from an HTTP URL to the I2S
amp. So the laptop runs a two-line local HTTP server, writes the generated MP3 there, and
sends the ESP32 a URL. That is far less work than pushing audio bytes yourself.

**This means WiFi, not serial.** Streaming MP3 over USB serial is painful, so the audio path
needs a network. Do **not** use venue WiFi — use a **phone hotspot or your own travel
router**. Hackathon 2.4GHz is a warzone and captive portals eat demo time. Fallback if the
network dies: send MP3 bytes over serial into PSRAM and play from memory (~480KB for a 30s
track, ~5s transfer at 921600 baud — works, just adds latency).

---

## Backend contract

**Plain HTTP, request/response. No websocket for the core loop.** The device always
initiates, so there is nothing for the backend to push and nothing to keep alive. One
endpoint does the whole job.

```
POST /utterance
  body:     raw 16kHz 16-bit mono WAV (the PSRAM buffer)
  response: { "say": "here's something chill",
              "screen": "chill lo-fi",
              "audio_url": "http://<laptop>:8000/track_07.mp3",
              "volume": 0.6 }
```

The device does one thing with that response: show `screen`, set volume, and hand
`audio_url` to `ESP32-audioI2S`. All the intelligence stays on the laptop, which is also
the only place session state lives.

Backend pipeline for one request:

```
WAV ──► speech-to-text ──► Claude(utterance + current TrackSpec)
                                      │
                                      ▼ intent
              set_volume ─────────────┤  no generation, respond immediately
              transport ──────────────┤  no generation, respond immediately
              new_track / modify ─────┴─► ElevenLabs /v1/music
                                              │
                                          write MP3 into the static dir
                                              │
                                          respond with its URL
```

`GET /track_NN.mp3` is just `http.server` pointed at a folder. Two lines.

**Volume and transport don't generate.** They return in milliseconds because they never
touch ElevenLabs — which is exactly why the intent classification has to happen before
anything else, not after.

**The device blocks on this request.** A new_track response won't arrive for however long
generation takes, so the firmware must keep the screen alive and not trip its HTTP timeout.
Set the client timeout generously (60s+) and show a working state while waiting.

---

## Conversational context — the interesting part

This is what makes it feel AI-native rather than a voice-triggered jukebox.

### "Quieter" is ambiguous and you must resolve it

*"Make it quieter"* means either:
- **turn down playback volume** — instant, free, no API call
- **make the music calmer** — regeneration, slow, spends credits

If the device guesses wrong it feels broken. So the AI's job is not "write a music prompt" —
it's **classify the intent first**, then act. Ask Claude for structured output
(`output_config.format` with a JSON schema, so it's guaranteed parseable) with an intent enum:

```json
{
  "intent": "set_volume | modify_track | new_track | transport",
  "volume_delta": -0.2,
  "track": {
    "prompt": "downtempo lo-fi, warm rhodes, soft brushed drums",
    "duration_ms": 30000,
    "instrumental": true
  },
  "say": "turning it down"
}
```

Rule of thumb for the system prompt: **words about loudness → volume; words about
mood, energy, instruments, or genre → regeneration.** "Quieter", "turn it down", "too loud"
are volume. "Calmer", "softer", "more energetic", "add drums", "less busy" are modify_track.
When genuinely ambiguous, prefer volume — it's instant and free, and the user will correct it.

### Keeping context across turns

Hold a small session state on the laptop:

```python
current = TrackSpec(prompt=..., duration_ms=..., instrumental=..., seed=...)
history = [...]   # last few utterances
```

On every voice command, send Claude **the current TrackSpec plus the utterance**, and ask for
the *new* TrackSpec. So "make it more energetic" returns the same spec with the prompt
adjusted — not a fresh unrelated track. That's what "keeps the subject's context" means in
practice: the AI is editing a document, not answering a question.

**Reuse the `seed`.** The ElevenLabs music endpoint takes a `seed` for reproducibility. Keep
the seed constant across modify_track calls and you get recognisably *the same song, adjusted*
rather than a completely different one. This is the trick that makes the feature land.

For structural edits later, `composition_plan` (instead of a flat `prompt`) gives
section-by-section control, and `store_for_inpainting: true` enables editing a generated
track afterwards. Don't reach for either until the flat-prompt version works.

### Regeneration is slow — hide it

A modify_track is another full generation, so it has the same latency problem as a new track.
Three mitigations, all cheap:

1. **Keep playing the old track while the new one generates**, then crossfade when ready.
   Never drop to silence.
2. **Speak the `say` field** through ElevenLabs TTS ("making it warmer…") so the wait has a voice.
3. **Prefetch.** While a track plays, generate the next variant in the background so
   swipe-to-next is instant — exactly the moment gesture control needs to feel good.

**Measure the actual generation time first thing on the day.** The whole UX budget depends on
that number and we haven't measured it. Shorter tracks generate faster; consider 15–20s
tracks for a tighter demo loop.

---

## Later: the three-device split

Not building this now. Recorded so the prototype doesn't paint us into a corner.

| Device | Has | Does |
|---|---|---|
| **Output** | ESP32 + MAX98357A + speaker + IPS screen | Plays audio, shows state |
| **Input** | ESP32 + I2S mic + buttons | Push-to-talk, transport |
| **Vision** | ESP32 + ToF (or camera) | Hand detection → volume, next |

All three on WiFi to the laptop hub. It's a packaging change, not an architecture change —
the laptop stays the brain and the single owner of session state either way.

**The one thing to do now:** use addressed messages from day one — `input.button_down`,
`vision.distance`, `output.set_state` — rather than a flat protocol. Costs nothing today,
and the split becomes almost free later.

---

## Open source

Nothing here needs writing from scratch.

**Laptop (Python):**

| Job | Use |
|---|---|
| Speech → text | **ElevenLabs Scribe** (credits) or **faster-whisper** (local, no network) |
| Intent + prompt | **Anthropic SDK**, `claude-opus-4-8`, structured outputs |
| Music | **ElevenLabs** `POST /v1/music` — `prompt`, `music_length_ms` (3s–600s), `model_id` (`music_v1`/`music_v2`), `force_instrumental`, `seed` |
| Spoken replies | **ElevenLabs TTS** |
| MP3 hosting | `http.server` — two lines |
| Device comms | `websockets` or plain UDP |

**ESP32 (Arduino):**

| Job | Use |
|---|---|
| MP3 → I2S | `ESP32-audioI2S` (schreibfaul1) |
| I2S mic capture | ESP-IDF `driver/i2s` |
| Screen | `TFT_eSPI` or `LovyanGFX` |
| ToF | `Adafruit_VL53L0X` |

---

## Build order

Each step ends with something demoable. Don't advance until the current one works.

1. **Capture.** Button → I2S mic → PSRAM → level meter on screen. No network, no AI.
   Proves the mic is actually recording. *(firmware/soundbud)*
2. **Sound out of the speaker.** Device streams a hardcoded MP3 URL to the MAX98357A.
   The riskiest remaining hardware path — I2S out, amp, speaker, WiFi all at once.
3. **Music from a hardcoded prompt.** Laptop → ElevenLabs → local HTTP → device plays it.
   **Time the generation call and write the number down.**
4. **Wire the two halves.** Device POSTs its WAV to `/utterance`, backend transcribes and
   echoes the text back to the screen. Still no AI, no generation — this proves the
   transport, which is where the fiddly bugs live.
5. **Full loop.** Add Claude + ElevenLabs behind `/utterance`. This is the prototype.
6. **Context.** Session TrackSpec + intent classification. "Make it more energetic" works.
7. **Gestures.** ToF hover-for-volume, chop-for-next.
8. **Polish.** Prefetch, crossfade, spoken filler. This is what turns it into a good demo.
9. *Then:* split into three devices.

---

## Open questions

- Do we have a digital I2S mic (INMP441/ICS-43434) and a passive speaker, or do we need to source them?
- Phone hotspot or travel router for the device network?
- Track length? Shorter = faster generation = tighter loop.
- Does "next track" mean a fresh generation, or a queue of pre-generated variants?
- Second ToF sensor available (unlocks real left/right swipe)?
