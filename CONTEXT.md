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
| Brain | **PIXIE M1** (ESP32-S3) | WiFi + PSRAM. The only board with confirmed specs. |
| Screen | **IPS LCD 0.96"** | ~160×80. Design for four words, not a dashboard. |
| Amp | **Audio Amplifier (MAX98357A)** | I2S digital amp. |
| Speaker | — | **Not in the kit. Bring a passive 4–8Ω speaker.** |
| Mic | **Analog Microphone** in kit | We want a **digital I2S mic** instead — see below. |
| Hand detection | **Distance Sensor (ToF VL53L0X)** | There is no gesture sensor in the catalogue — see below. |

Also available: accelerometers (LSM6DS3TR / ICM-20948 / MPU6050), NeoPixels, vibration
motor, touchpad (TTP223 — good as the push-to-talk button), light and color sensors.

### Digital mic — bring an INMP441 or ICS-43434

The kit mic is **analog** (ESP32 ADC). Noisy, and speech-to-text degrades badly with it in
a loud room. A **digital I2S mic** (INMP441 is the common cheap one, ICS-43434 is better)
plugs into the ESP32-S3's I2S peripheral and gives clean 16kHz mono with no analog stage.
This is the single most important part to bring — everything downstream depends on the
speech being transcribable.

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
model — and it never misfires while someone is talking over you on stage. The ESP32-S3 has
8MB PSRAM; 10 seconds of 16kHz mono 16-bit is 320KB, which fits with room to spare.

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

1. **Sound out of the speaker.** ESP32 streams any hardcoded MP3 URL to the MAX98357A.
   Proves I2S, amp, speaker, and WiFi in one shot — and it's the riskiest hardware path,
   so do it first.
2. **Music from a hardcoded prompt.** Laptop → ElevenLabs → local HTTP → device plays it.
   **Time the generation call and write the number down.**
3. **Voice in.** Button → I2S mic → PSRAM → WAV to laptop → transcript on screen. No AI yet.
4. **Full loop.** Voice → Claude → ElevenLabs → device plays it. This is the prototype.
5. **Context.** Session TrackSpec + intent classification. "Make it more energetic" works.
6. **Gestures.** ToF hover-for-volume, chop-for-next.
7. **Polish.** Prefetch, crossfade, spoken filler. This is what turns it into a good demo.
8. *Then:* split into three devices.

---

## Open questions

- Do we have a digital I2S mic (INMP441/ICS-43434) and a passive speaker, or do we need to source them?
- Phone hotspot or travel router for the device network?
- Track length? Shorter = faster generation = tighter loop.
- Does "next track" mean a fresh generation, or a queue of pre-generated variants?
- Second ToF sensor available (unlocks real left/right swipe)?
