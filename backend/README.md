# soundBud backend

Voice in, generated music out. Transcribe → decide what the user meant → generate if needed.

---

## Run it

```powershell
copy .env.example .env      # then put your keys in it (repo root, gitignored)

cd backend
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000
```

**`--host 0.0.0.0` is not optional.** On `127.0.0.1` the ESP32 gets connection-refused
while `curl localhost:8000` works perfectly from this machine. It is the most common
failure and it looks exactly like a firmware bug.

### Before the device is involved

1. **Same network.** Phone hotspot. Laptop and ESP32 both on it. Not venue WiFi.
2. **Find this machine's IP** — `ipconfig`, the IPv4 address on the hotspot adapter
   (something like `192.168.43.x`). That is what the firmware needs.
3. **Allow it through the firewall.** Windows pops a dialog on first run — tick
   **Private networks**. Miss it and the port is silently closed.
4. **Prove the path from your phone's browser:** open `http://192.168.43.x:8000/`. You
   should see `{"ok":true,...}`. If that loads, network + bind address + firewall are all
   proven and anything that fails next is firmware.
5. **Test the whole pipeline with the laptop as the device:**

   ```powershell
   python talk.py                       # backend on localhost
   python talk.py http://192.168.43.5:8000
   ```

   Records from the laptop mic, POSTs, plays whatever comes back — the same contract the
   firmware will use. Enter starts recording, Enter stops.

   Try, in order: *"play something chill with no vocals"* → *"make it more energetic"*
   (should regenerate, same seed) → *"turn it down"* (should be instant, no new audio).
   That sequence exercises all three intents and the conversational context.

Run `python main.py` to exercise the state logic (seed reuse, volume clamping) with no
network and no keys.

---

## Device contract

One endpoint. The device always initiates — there is nothing to push and no websocket.

### `POST /utterance`

**Request:** raw 16kHz 16-bit mono WAV as the body. No multipart, no JSON wrapper —
just the PSRAM buffer's bytes.

**Response:**

```json
{
  "say": "here's something chill",
  "screen": "chill lo-fi",
  "audio_url": "http://192.168.43.5:8000/tracks/track_1763481200.mp3",
  "volume": 0.6
}
```

| Field | Do this with it |
|---|---|
| `say` | Optional. Speak or ignore — nothing breaks if unused. |
| `screen` | Show on the display. Already truncated to 20 chars. |
| `audio_url` | **May be `null`.** If present, stream it to the MAX98357A. If null, there is no new audio — just update the screen and volume. |
| `volume` | 0.0–1.0. Apply to playback. |

### `GET /tracks/<name>.mp3`

The generated audio. MP3, 44.1kHz, 128kbps. `audio_url` is already an absolute URL
pointing at the address the device reached us on, so **there is no host to configure on
the device beyond the one it already POSTs to.**

---

## Firmware gotchas

**Set the HTTP timeout to 60 seconds or more.** `audio_url` responses are slow — the
backend is generating music before it can reply. A default 5–10s timeout will abandon
requests that were about to succeed. Keep the screen on a working state for the whole wait.

**`audio_url` being `null` is normal, not an error.** "Turn it down" and "skip" return in
milliseconds with no audio because they never touch the music API. Only a request that
changes the *music* generates.

**Nothing is streamed up.** Record the full utterance into PSRAM on button-hold, then POST
the whole buffer once on release. No chunking, no keep-alive.
