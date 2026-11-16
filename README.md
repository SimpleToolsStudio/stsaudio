# STSAudio

[![PyPI version](https://img.shields.io/pypi/v/stsaudio.svg)](https://pypi.org/project/stsaudio/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**STSAudio** is a single-file CPython extension that provides a complete audio playback library for Python, powered by the excellent [miniaudio](https://github.com/mackron/miniaudio) and [minimp3](https://github.com/lieff/minimp3) single-header libraries.

It supports playing **MP3**, **WAV**, and **FLAC** files with zero external dependencies.

## Features

- **Zero dependencies**: Everything is compiled with MSVC and linked directly into a single `.pyd` file.
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

# Load a sound and get its ID (supports up to 64 simultaneous sounds)
sid = audio.sound_load("path/to/your/music.mp3")

# Control playback
audio.sound_play(sid)
audio.sound_set_volume(sid, 0.5)  # 50% volume
audio.sound_set_looping(sid, True)

print("Duration:", audio.sound_get_duration(sid), "seconds")

# Stop and unload
audio.sound_stop(sid)
audio.sound_unload(sid)

audio.engine_uninit()
```

## API Reference

STSAudio exposes 21 high-level functions for common audio tasks:

- **Information**: `version()`
- **Decoding**: `decode_file()`, `decode_memory()`
- **Blocking Playback**: `play_file()`
- **Engine**: `engine_init()`, `engine_uninit()`, `engine_set_volume()`, `engine_stop_all()`
- **Sound Management**: `sound_load()`, `sound_play()`, `sound_stop()`, `sound_pause()`, `sound_resume()`, `sound_unload()`
- **Sound Properties**: `sound_set_volume()`, `sound_get_volume()`, `sound_set_looping()`, `sound_is_playing()`
- **Playback Position**: `sound_seek()`, `sound_get_duration()`, `sound_get_position()`

### Low-Level Access
For advanced use cases, all miniaudio and minimp3 functions are exposed as `fn_*` PyCapsules. You can retrieve the raw C function pointers using `ctypes`.

## Building from Source

*Requirements: MSVC (Visual Studio 2022 Build Tools) and Python 3.14 (64-bit).*

1. Clone the repository.
2. Download headers: run `Download-HeaderFile.bat` (or manually download `miniaudio.h`, `minimp3.h`, and `minimp3_ex.h`).
3. Compile the `.pyd`:
   ```cmd
   build-pyd.bat
   ```
4. Build the wheel:
   ```cmd
   python build_wheel.py
   ```

## License

This project is licensed under the **MIT License**.
It uses:
- [miniaudio](https://github.com/mackron/miniaudio) (Public Domain / MIT-0)
- [minimp3](https://github.com/lieff/minimp3) (CC0)

Copyright (c) 2026 SimpleToolsStudio
```