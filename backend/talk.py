"""Laptop stand-in for the Axiometa device.

Records from the laptop mic, POSTs to the backend, plays whatever comes back.
Same contract the firmware will use, so testing here proves the pipeline
before any hardware exists.

    python talk.py                       # backend on localhost
    python talk.py http://192.168.43.5:8000

Enter starts recording, Enter stops. Ctrl-C quits.
"""

import io
import sys
import wave

import httpx
import numpy as np
import pygame
import sounddevice as sd

BACKEND = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8000"
RATE = 16000  # what speech-to-text wants, and what the device will send

# Windows consoles default to cp1251 here; an emoji from the model would
# otherwise crash the client on print.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def record() -> bytes:
    """Record until the user hits Enter again. Returns a WAV."""
    chunks = []
    with sd.InputStream(samplerate=RATE, channels=1, dtype="int16",
                        callback=lambda data, *_: chunks.append(data.copy())):
        input("  recording — Enter to stop... ")

    if not chunks:
        return b""

    audio = np.concatenate(chunks)
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(audio.tobytes())
    return buffer.getvalue()


def play(url: str, volume: float) -> None:
    mp3 = httpx.get(url, timeout=60.0).content
    pygame.mixer.music.load(io.BytesIO(mp3), "mp3")
    pygame.mixer.music.set_volume(volume)
    pygame.mixer.music.play()


def main() -> None:
    pygame.mixer.init()
    print(f"backend: {BACKEND}\n")

    while True:
        input("Enter to talk... ")
        wav = record()
        if not wav:
            print("  nothing captured — is the mic working?\n")
            continue

        print(f"  sent {len(wav) // 1024}KB, waiting...")
        # Generous: the backend generates music before it replies. The firmware
        # needs the same, and a short timeout here would abandon a good request.
        response = httpx.post(f"{BACKEND}/utterance", content=wav, timeout=180.0)
        response.raise_for_status()
        reply = response.json()

        print(f"  screen : {reply['screen']}")
        print(f"  say    : {reply['say']}")
        print(f"  volume : {reply['volume']}")

        if reply["audio_url"]:
            print(f"  playing: {reply['audio_url']}")
            play(reply["audio_url"], reply["volume"])
        else:
            # No new audio: a volume or transport command. Apply it to whatever
            # is already playing, exactly as the device will.
            pygame.mixer.music.set_volume(reply["volume"])
            print("  (no new track — adjusted what's playing)")
        print()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")
