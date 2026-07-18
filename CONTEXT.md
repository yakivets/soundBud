# soundBud — AI-native music station

You talk to it, it generates music, it plays on its own speaker. Follow-up commands modify
the track you're hearing instead of starting over. Built at Axiometa London hackathon.

**Status:** design context. Nothing built yet.

**Scope: the prototype.** One device — voice in, generated song out of its own speaker,
follow-up commands that modify what's playing. The eventual three-device split (output /
input / vision) is noted at the bottom but is not what we're building.

---

## Hardware

From the Axiometa catalogue (checked against the live product pages):

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
