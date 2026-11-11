# Duke Nukem 3D as a B310E OS app: $(BUILD)/ports/duke3d.app (included by os/Makefile
# after the kit; `make duke3d`, and part of `make` once the source is there). The Build
# engine's side is shared (ports/build/build.mk); here the game: its source (python
# os/ports/sources.py jfduke3d), the changes prepare.py makes, what build_game.h asks of
# it (duke_game.c), its OS side (duke_os.c). README.md here says what licences this makes.
#
# SPDX-License-Identifier: GPL-2.0-or-later

include ports/build/build.mk

duke_GAME ?= ../build/jfduke3d
duke_GAME_SRC := game actors gamedef global menues player premap sector rts config osdfuncs \
	osdcmds grpscan sounds version
duke_PORT := ports/duke
duke_C := duke_game
duke_APP := duke3d

# the LCD's 160 x 100, drawn 1:1 (layer.c)
duke_DEFS := -DBUILD_WIDTH=160 -DBUILD_HEIGHT=100
$(eval $(call build_game,duke))
