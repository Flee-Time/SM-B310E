# Blood

`blood.app`: NBlood's game code on the shared Build engine (`os/ports/build`, jfbuild),
the way fpdoom runs it. The sources stay outside the repository, fetched at build time
(`python os/ports/sources.py jfbuild jfmact jfaudiolib nblood fpdoom`); `prepare.py`
says what it changes.

It needs Blood v1.21's files (One Unit Whole Blood; GOG's installer has them): the
`.RFF`, `TILES0*.ART`, `.DAT` and `BLOOD.INI` files, and for Cryptic Passage its `CP*.MAP`
and `CPART*.AR_`. Older releases (1.00, 1.1x) number their animations differently and
do not load. They go beside the app (`/b310e-os/apps/Games/Blood/`) or in `/games/blood`.

## Licences

- NBlood's Blood code and MACT are GPLv2. fpdoom's adaptation (`fpbuild/NBlood.patch`,
  `eduke32/`, parts of `jfbuild.patch`) is public domain. The engine is under Ken
  Silverman's Build licence, as for Duke (`os/ports/duke/README.md`): a built
  `blood.app` may be passed on only as that licence says (free, with `buildlic.txt`).
- The code here is GPLv2+, with the extra permission in `os/LICENSE`.
- The game's data is Monolith's and is yours to supply.
