#!/usr/bin/env python3
"""make_audio.py — генерирует звуки игры в game/assets/music.

Ничего внешнего не качаем: и щелчок кнопки, и фоновая мелодия синтезируются
здесь же обычной математикой, поэтому лицензия у них наша и весят они копейки.

    python3 tools/make_audio.py

Файлы на выходе (44100 Гц, моно, 16 бит):
    game/assets/music/click.wav  — щелчок по кнопке
    game/assets/music/music.wav  — зацикленная фоновая мелодия

Свои файлы можно просто положить сверху с теми же именами: движок грузит их
по имени, формат — WAV PCM 16 бит.
"""
from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

RATE = 44100
OUT = Path(__file__).resolve().parent.parent / "game" / "assets" / "music"


def write_wav(path: Path, samples: list[float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for s in samples:
        v = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(v * 32000))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"{path}  {len(samples) / RATE:.2f} c")


def click() -> list[float]:
    """Короткий сухой щелчок: две быстро затухающие синусоиды плюс лёгкий шум."""
    n = int(RATE * 0.07)
    out = []
    seed = 12345
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * 55.0)
        body = math.sin(2 * math.pi * 880.0 * t) * 0.55
        tick = math.sin(2 * math.pi * 1760.0 * t) * 0.25 * math.exp(-t * 120.0)
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        noise = ((seed / 0x7FFFFFFF) * 2.0 - 1.0) * 0.12 * math.exp(-t * 200.0)
        out.append((body + tick + noise) * env * 0.8)
    # мягкий хвост, чтобы не было щелчка от обрыва
    tail = int(RATE * 0.005)
    for i in range(tail):
        out[-tail + i] *= 1.0 - i / tail
    return out


def note(freq: float, start: float, dur: float, buf: list[float], gain: float,
         kind: str = "tri", attack: float = 0.01) -> None:
    """Кладёт ноту в общий буфер: tri — мягкий лид, sqr — бас, sin — подклад."""
    a = int(start * RATE)
    n = int(dur * RATE)
    for i in range(n):
        if a + i >= len(buf):
            break
        t = i / RATE
        phase = (freq * (a + i) / RATE) % 1.0
        if kind == "tri":
            v = 4.0 * abs(phase - 0.5) - 1.0
        elif kind == "sqr":
            v = 1.0 if phase < 0.5 else -1.0
        else:
            v = math.sin(2 * math.pi * phase)
        env = min(1.0, t / attack) * math.exp(-t * (1.6 if kind != "sqr" else 2.2))
        buf[a + i] += v * env * gain


def music() -> list[float]:
    """Спокойный луп: Am - F - C - G, бас плюс арпеджио. Ровно 16 секунд."""
    bpm = 84.0
    beat = 60.0 / bpm
    bars = 8
    total = beat * 4 * bars
    buf = [0.0] * int(total * RATE)

    # Ноты: частоты равномерного строя.
    def f(semitone: float) -> float:
        return 440.0 * (2.0 ** (semitone / 12.0))

    # Am, F, C, G — по два такта на аккорд, круг повторяется дважды.
    chords = [
        (f(-12), [f(0), f(3), f(7)]),      # Am
        (f(-16), [f(-4), f(0), f(5)]),     # F
        (f(-21), [f(-5), f(0), f(4)]),     # C
        (f(-14), [f(-1), f(2), f(7)]),     # G
    ]
    for bar in range(bars):
        root, chord = chords[bar % len(chords)]
        bar_t = bar * beat * 4
        # Бас на сильные доли.
        note(root, bar_t, beat * 1.6, buf, 0.22, "sqr", 0.02)
        note(root, bar_t + beat * 2, beat * 1.6, buf, 0.18, "sqr", 0.02)
        # Арпеджио восьмыми.
        for step in range(8):
            n = chord[step % len(chord)]
            if step in (3, 7):
                n *= 2.0
            note(n, bar_t + step * beat * 0.5, beat * 0.6, buf, 0.10, "tri", 0.006)
        # Тихая подушка.
        note(chord[0] / 2.0, bar_t, beat * 3.6, buf, 0.05, "sin", 0.25)

    # Нормализация и бесшовная склейка: конец переливается в начало.
    peak = max(0.001, max(abs(v) for v in buf))
    buf = [v / peak * 0.72 for v in buf]
    fade = int(RATE * 0.12)
    for i in range(fade):
        k = i / fade
        head = buf[i]
        tail = buf[len(buf) - fade + i]
        buf[i] = head * k + tail * (1.0 - k)
        buf[len(buf) - fade + i] = tail * (1.0 - k) + head * k
    return buf


if __name__ == "__main__":
    write_wav(OUT / "click.wav", click())
    write_wav(OUT / "music.wav", music())
