# waybar-vis

A lightweight PipeWire audio spectrum visualizer for [Waybar](https://github.com/Alexays/Waybar).

Captures system audio via PipeWire, runs FFT analysis, and outputs Unicode block characters as JSON — ready to drop into a Waybar custom module.

## Dependencies

- `pipewire` (libpipewire-0.3)
- `fftw` or built-in FFT (see `fft.c`)
- `waybar` (for display)

## Build

```sh
make
sudo make install
```

## Usage

```sh
waybar-vis [-b bands] [-r rate] [-t threshold]
```

| Flag | Default | Description |
|------|---------|-------------|
| `-b` | 16      | Number of frequency bands (4–64) |
| `-r` | 30      | Refresh rate in Hz (10–60) |
| `-t` | 0.08    | Silence threshold (peak band value) |

### Waybar config

```json
"custom/vis": {
    "exec": "~/.local/bin/waybar-vis -b 20",
    "tail": true,
    "return-type": "json",
    "format": "{}",
    "tooltip": true
}
```

### CSS

```css
#custom-vis {
  font-family: monospace;
  font-size: 12px;
  color: #dddddd;
  letter-spacing: 1px;
  min-width: 80px;
}
#custom-vis.hidden {
  min-width: 0;
  min-height: 0;
  padding: 0;
  margin: 0;
  font-size: 0;
  opacity: 0;
  border: none;
}
```

The `.hidden` class is applied automatically when no audio is playing.

## How it works

1. Captures audio from the system audio sink via PipeWire
2. Maintains a ring buffer of PCM samples
3. On each timer tick, runs FFT and splits frequencies into logarithmic-scale bands
4. Smooths values with attack/decay for a fluid visual
5. Outputs JSON with Unicode block characters (▁▂▃▄▅▆▇█)
6. Auto-hides after 8 consecutive silent frames
