# doom-braille — Doom in the terminal, drawn with braille characters.

DOOMGENERIC_COMMIT := dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
DOOMGENERIC_REPO   := https://github.com/ozkl/doomgeneric.git
DOOMGENERIC_DIR    := third_party/doomgeneric
ENGINE_SRC_DIR     := $(DOOMGENERIC_DIR)/doomgeneric

TEST_WAD_URL    := https://raw.githubusercontent.com/nneonneo/universal-doom/fcf22d9773c62835724f2d25c40933ef051f6600/DOOM1.WAD
TEST_WAD_SHA256 := 1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
TEST_WAD        := .cache/DOOM1.WAD

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE
BUILD   := build

# Every renderer source except the doomgeneric port goes into the CLI.
CLI_SRC := $(filter-out src/doomgeneric_braille.c,$(wildcard src/*.c))

# Backend files doomgeneric ships for platforms/toolkits we don't use
# (SDL, Allegro, X11, framebuffer, emscripten, Windows): our own
# src/doomgeneric_braille.c (patches/, T-005) is the only backend we build.
ENGINE_EXCLUDE := doomgeneric_sdl.c doomgeneric_xlib.c doomgeneric_allegro.c \
                   doomgeneric_emscripten.c doomgeneric_linuxvt.c \
                   doomgeneric_soso.c doomgeneric_sosox.c doomgeneric_win.c \
                   i_joystick.c i_sdlsound.c i_sdlmusic.c \
                   i_allegrosound.c i_allegromusic.c i_cdmus.c
ENGINE_SRC := $(filter-out $(addprefix $(ENGINE_SRC_DIR)/,$(ENGINE_EXCLUDE)),$(wildcard $(ENGINE_SRC_DIR)/*.c))
ENGINE_OBJ := $(patsubst $(ENGINE_SRC_DIR)/%.c,$(BUILD)/engine/%.o,$(ENGINE_SRC))

# WAD selection: WAD= wins, else the first *.wad in WADS_DIR (tests point
# WADS_DIR at an empty scratch directory to exercise the "no WAD" error
# without touching the real wads/ folder).
WAD ?=
WADS_DIR ?= wads
ifeq ($(strip $(WAD)),)
WAD_CANDIDATE := $(firstword $(wildcard $(WADS_DIR)/*.wad) $(wildcard $(WADS_DIR)/*.WAD))
else
WAD_CANDIDATE := $(WAD)
endif

.PHONY: run build cli engine game fetch-doomgeneric fetch-wad check-wad \
        test-deps test clean _engine-objects _game-link

run: check-wad game
	./$(BUILD)/braille-game -iwad "$(WAD_CANDIDATE)"

build: cli game

cli: $(BUILD)/braille-cli

$(BUILD)/braille-cli: $(CLI_SRC) $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	@if [ -z "$(strip $(CLI_SRC))" ]; then \
	  echo "No renderer sources yet (src/*.c) — nothing to build for braille-cli."; \
	else \
	  $(CC) $(CFLAGS) -o $@ $(CLI_SRC) -lm; \
	fi

# Everything the game binary needs except the CLI's own main().
GAME_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(filter-out src/braille_cli.c,$(wildcard src/*.c)))

# $(ENGINE_OBJ) is only correct once third_party/doomgeneric exists, so (like
# `engine`) this re-invokes make after fetch-doomgeneric has run.
game: fetch-doomgeneric
	@$(MAKE) --no-print-directory _game-link

_game-link: $(BUILD)/braille-game

$(BUILD)/braille-game: $(GAME_OBJ) $(ENGINE_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(GAME_OBJ) $(ENGINE_OBJ) -lm

$(BUILD)/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(ENGINE_SRC_DIR) -c -o $@ $<

# Checked out fresh on first use, git-ignored; never edited in place.
fetch-doomgeneric:
	@if [ ! -d $(DOOMGENERIC_DIR)/.git ]; then \
	  echo "Fetching doomgeneric..."; \
	  mkdir -p third_party; \
	  git clone --quiet $(DOOMGENERIC_REPO) $(DOOMGENERIC_DIR); \
	fi; \
	current=$$(git -C $(DOOMGENERIC_DIR) rev-parse HEAD 2>/dev/null); \
	if [ "$$current" != "$(DOOMGENERIC_COMMIT)" ]; then \
	  git -C $(DOOMGENERIC_DIR) fetch --quiet origin $(DOOMGENERIC_COMMIT) 2>/dev/null || true; \
	  git -C $(DOOMGENERIC_DIR) checkout --quiet $(DOOMGENERIC_COMMIT); \
	fi

# The object list depends on files that fetch-doomgeneric just created, so
# it is computed by a fresh `make` invocation, after the clone/checkout above.
engine: fetch-doomgeneric
	@$(MAKE) --no-print-directory _engine-objects

_engine-objects: $(ENGINE_OBJ)

$(BUILD)/engine/%.o: $(ENGINE_SRC_DIR)/%.c
	@mkdir -p $(BUILD)/engine
	$(CC) $(CFLAGS) -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$(ENGINE_SRC_DIR) -c -o $@ $<

fetch-wad: $(TEST_WAD)

$(TEST_WAD):
	@mkdir -p .cache
	@echo "Fetching test WAD (shareware DOOM1.WAD)..."
	@curl -sL --fail -o $@.tmp $(TEST_WAD_URL)
	@got=$$(sha256sum $@.tmp | cut -d' ' -f1); \
	if [ "$$got" != "$(TEST_WAD_SHA256)" ]; then \
	  echo "error: test WAD sha256 mismatch (got $$got, expected $(TEST_WAD_SHA256))" >&2; \
	  rm -f $@.tmp; \
	  exit 1; \
	fi
	@mv $@.tmp $@

check-wad:
	@if [ -z "$(WAD_CANDIDATE)" ]; then \
	  echo "error: no WAD found. Put a Doom-engine .wad file in wads/, or run 'make run WAD=path/to/file.wad'" >&2; \
	  exit 1; \
	fi; \
	if [ ! -f "$(WAD_CANDIDATE)" ]; then \
	  echo "error: WAD file not found: $(WAD_CANDIDATE)" >&2; \
	  exit 1; \
	fi; \
	echo "Using WAD: $(WAD_CANDIDATE)"

clean:
	rm -rf $(BUILD)
