# doom-braille

Doom, running in your terminal, drawn entirely with Unicode braille
characters (`⠀`–`⣿`). Each character packs a 2x4 grid of dots, so a normal
terminal window becomes a surprisingly detailed picture.

It plays: WASD and the arrow keys move and turn the marine, Space fires,
E opens doors and flips switches. See `.spec/features/braille-renderer/spec.md`
for the full rendering algorithm and `.spec/features/terminal-input/spec.md`
for the keyboard handling. Sound is not wired up yet (phase 3).

## Requirements

- A C compiler (`cc`/`gcc`), `make`, `git`, `curl`.
- A Doom-engine WAD file. The shareware `DOOM1.WAD` works.
- Node.js, only to run the test suite.

## Running it

```sh
make run
```

This fetches doomgeneric (pinned to a specific commit, never modified in
place — any patch lives in `patches/`), builds the game, and starts it
using a WAD from `wads/`.

### Picking a WAD

`make run` uses the first `*.wad` file it finds in `wads/`. Drop your WAD
there:

```sh
cp ~/Downloads/DOOM1.WAD wads/
make run
```

Or point at any file directly:

```sh
make run WAD=/path/to/doom.wad
```

If no WAD can be found, `make run` exits with a non-zero status and an
error explaining where to put one.

## Controls

| Key                                                                       | Action                                                                  |
| ------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| `W` / `S` or `↑` / `↓`                                          | move forward / backward (arrows also navigate menus and the automap)    |
| `A` / `D`                                                             | strafe left / right                                                     |
| `←` / `→`                                                           | turn left / right (also menu left/right)                                |
| `Space`                                                                 | fire (`Ctrl` too, on a terminal with Kitty keyboard protocol support) |
| `E`                                                                     | use (doors, switches)                                                   |
| `Shift`                                                                 | run                                                                     |
| `1`–`7`                                                              | weapons                                                                 |
| `Esc`, `Enter`, `Tab`, `Backspace`, `F1`–`F12`, `-`, `=` | the engine's own menu/automap/confirm keys                              |
| any other letter or digit                                                 | passed straight to the engine (cheats, save names,`y`/`n` prompts)  |
| `Alt+C`                                                                 | toggle color                                                            |
| `Alt+I`                                                                 | toggle invert                                                           |
| `Ctrl+C`                                                                | quit at once, terminal restored                                         |

With color off, every glyph is drawn in a fixed light gray (`#D0D0D0`), so
the picture looks the same before and after an `Alt+C` round trip. Pick a
different one with `MONO_COLOR`, for example `MONO_COLOR=33FF66 make run`.
Shading uses a symmetric dot ladder: dark tones light 1, 2, 4, ... of every
256 dots, and bright tones leave 1, 2, 4, ... unlit.
The engine's startup log is kept off the game screen and printed to the
terminal after you quit.

On a terminal that supports the [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/),
key releases are reported directly, so held keys behave exactly as expected.
On every other terminal, a key counts as held while its bytes keep arriving
and as released once none arrive for `KEY_RELEASE_MS` milliseconds (default
100) — tune it if your terminal's own key-repeat rate needs it, for example
`KEY_RELEASE_MS=150 make run`.

## Other targets

- `make build` — compile the game without starting it.
- `make cli` — build `build/braille-cli`, a standalone tool that converts
  a single image to braille.
- `make clean` — remove build output.

## Project layout

- `src/` — the braille renderer, layout, terminal driver, and the
  doomgeneric port (`patches/` holds any diff against doomgeneric itself).
- `third_party/doomgeneric/` — fetched by `make`, git-ignored, pinned to
  the commit in `Makefile`.
- `wads/` — put your WAD file(s) here (git-ignored, except this note).
