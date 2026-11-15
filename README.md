# STSAudio

[![PyPI version](https://img.shields.io/pypi/v/stsaudio.svg)](https://pypi.org/project/stsaudio/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**STSAudio** is a single-file CPython extension that provides a complete audio playback library for Python, powered by the excellent [miniaudio](https://github.com/mackron/miniaudio) and [minimp3](https://github.com/lieff/minimp3) single-header libraries.

It supports playing **MP3**, **WAV**, and **FLAC** files with zero external dependencies.

## Features

- **Zero dependencies**: Everything is statically linked into a single `.pyd` file.
- **High-level APIs**: Simple functions to load, play, pause, seek, and control volume.
- **Low-level access**: Exposes over 1200 raw C APIs via `PyCapsule` for advanced users.
- **Permissive license**: MIT license (free for commercial and closed-source use).

## Installation

```bash
pip install stsaudio
```

*(Note: Currently, the pre-compiled binary wheel is provided for Windows 64-bit and Python 3.14.)*

## Quick Start

```python
import audio

# 1. Play an audio file (blocking)
audio.play_file("path/to/your/music.mp3")

# 2. Decode to raw PCM (16-bit, 2 channels)
pcm_data, frames, channels, sample_rate = audio.decode_file("path/to/your/music.mp3")
print(f"Decoded {frames} frames at {sample_rate} Hz")

# 3. Non-blocking playback engine
audio.engine_init()
sid = audio.sound_load("path/to/your/music.mp3")
audio.sound_play(sid)
audio.sound_set_volume(sid, 0.5)
audio.sound_set_looping(sid, True)
print("Duration:", audio.sound_get_duration(sid), "seconds")
audio.sound_stop(sid)
audio.sound_unload(sid)
audio.engine_uninit()
```

## License

This project is licensed under the **MIT License**.
It uses:
- [miniaudio](https://github.com/mackron/miniaudio) (Public Domain / MIT-0)
- [minimp3](https://github.com/lieff/minimp3) (CC0)

Copyright (c) 2026 SimpleToolsStudio
