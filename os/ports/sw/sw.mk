# Shadow Warrior as a B310E OS app: $(BUILD)/ports/sw.app (included by os/Makefile after
# the kit; `make sw`, and part of `make` once the source is there). The Build engine's side
# is shared (ports/build/build.mk); here the game: its source (python os/ports/sources.py
# jfsw), the changes prepare.py makes, what build_game.h asks of it (sw_game.c), its OS
# side (sw_os.c). ../duke/README.md says what licences a Build game makes.
#
# SPDX-License-Identifier: GPL-2.0-or-later

include ports/build/build.mk

sw_GAME ?= ../build/jfsw
sw_GAME_SRC := actor ai anim border break bunny cache cheats colormap config console coolg \
	coolie copysect demo draw eel game girlninj goro grpscan hornet interp interpsh inv \
	jplayer jsector jweapon lava light mclip menus miscactr morph net ninja \
	osdfuncs panel player predict quake ripper ripper2 rooms rotator rts save saveable \
	scrip2 sector serp setup skel skull slidor sounds spike sprite sumo swconfig sync \
	text track vator version vis wallmove warp weapon zilla zombie
sw_PORT := ports/sw
sw_C := sw_game
sw_APP := sw

$(eval $(call build_game,sw))
