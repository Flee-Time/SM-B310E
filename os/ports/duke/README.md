# Duke Nukem 3D

`duke3d.app`: upstream's jfduke3d, jfmact and jfbuild on the game kit. The sources stay
outside the repository, fetched at build time (`python os/ports/sources.py jfbuild jfmact
jfduke3d jfaudiolib`).

## Licences

- The game and MACT are GPLv2 (3D Realms). The engine is under Ken Silverman's Build
  licence (`buildlic.txt` in jfbuild), which only allows free, non-commercial
  distribution, over the internet, with that notice. It is not GPL-compatible: upstream's
  jfduke3d has that same combination.
- The code here and the game kit it links with (`os/ports/kit`) are GPLv2+, with the
  extra permission in `os/LICENSE`: they may be linked with the Build engine.
- So a built `duke3d.app` may be passed on only as the Build licence says: free, not
  for sale, with `buildlic.txt`. This repository holds no Build source and no binary.
- The game's data (`DUKE3D.GRP`) is 3D Realms' and is yours to supply.
