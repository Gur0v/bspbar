# bspbar

A port of [mangobar](https://github.com/Gur0v/mangobar) to X11 and `bspwm`, written in C.

![Preview](PREVIEW.png)

- Visible desktops on the left, click to focus, scroll to cycle
- Volume, keyboard layout, and clock on the right
- Source-level configuration via `config.h`

## Dependencies

```sh
sudo apt install libx11-dev libxcb1-dev libcairo2-dev libpango1.0-dev \
                 libxkbcommon-dev libxkbcommon-x11-dev libpipewire-0.3-dev bspwm
```

## Build and run

```sh
make
./bspbar
./bspbar --monitor HDMI-0  # pin to a specific monitor
```

## Notes

- Monitor space is reserved via `bspc bottom_padding` and restored on exit
- Keyboard layout is read through XKB directly
- Volume is read natively from PipeWire and matches `wpctl` output
- Configuration lives in [`config.h`](config.h)
