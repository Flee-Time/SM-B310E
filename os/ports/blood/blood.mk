# Blood as a B310E OS app: $(BUILD)/ports/blood.app (included by os/Makefile after the
# kit; `make blood`, and part of `make` once the source is there). NBlood's game code on
# the shared Build engine (ports/build/build.mk), as fpdoom runs it: the sources are
# upstream's, pinned and unchanged (python os/ports/sources.py jfbuild jfmact jfaudiolib
# nblood fpdoom), copied to the build with the changes prepare.py lists. Ours here: what
# build_game.h asks of the game (blood_keys.c, blood_game.cpp), its OS side (blood_os.c).
#
# SPDX-License-Identifier: GPL-2.0-or-later

include ports/build/build.mk

blood_GAME ?= ../build/nblood/source/blood
blood_MARK := src/blood.cpp
blood_GAME_CXX := actor ai aibat aibeast aiboneel aiburn aicaleb aicerber aicult aigarg \
	aighost aigilbst aihand aihound aiinnoc aipod airat aispid aitchern aizomba aizombf \
	blood callback choke common config db demo dude endgame eventq fire fx gamemenu \
	getopt gfx gib globals gui inifile iob levels loadsave map2d messages misc network \
	player qav qheap sectorfx seq sound sfx asound trig triggers warp weapon controls credits \
	gameutil menu mirrors osdcmd replace resource tile view screen compat
blood_GAME_SRC := version
blood_PORT := ports/blood
blood_C := blood_keys
blood_CPP := blood_game
blood_APP := blood
# fpdoom's switches for its adaptation (fpbuild/Makefile): no network, no float, the
# Amiga port's lighter ways, no movies. Upstream's char is unsigned (NBlood, fpdoom)
blood_DEFS := -include ports/blood/blood_int.h -Iports/blood -funsigned-char -DGAME_BLOOD -DEMBEDDED=1 -DNO_SOUND=0 -DNO_MOUSE=1 -DNO_NET=1 \
	-DNO_FLOAT=1 -DNETCODE_DISABLE -DSMACKER_DISABLE -D__AMIGA__ -DCXX_SUPPORT
blood_CXXDEFS := -Wno-char-subscripts -Wno-missing-braces

$(eval $(call build_game,blood))
