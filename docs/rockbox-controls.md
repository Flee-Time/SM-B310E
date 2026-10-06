# Rockbox controls on the SM-B310E

The directional pad, Center and soft keys cover normal navigation and
playback. The same physical buttons have these defaults:

| Button | While a track is displayed | In menus / file browser |
| --- | --- | --- |
| Center / OK | Play or pause | Select |
| Hold Center | Track context menu | Item context menu |
| Up / Down | Raise / lower volume; hold to repeat | Move up / down; hold to repeat |
| Left / Right | Previous / next track | Back / enter |
| Hold Left / Right | Seek backward / forward; release to resume | List scrolling, according to the list setting |
| Left soft key | Main menu | Main menu |
| Right soft key | Back to file browser; playback continues | Back / cancel |
| DIAL | Play or pause | Return to the playing track |
| END | Stop playback | Stop / cancel |
| Hold END | Shut down | Shut down |
| Hold STAR | Quickscreen | Quickscreen |

Rockbox's previous-track behavior can restart the current track if it has
already played past its configured skip threshold. Release after seeking
finishes the seek; it does not also skip a track. The number-key shortcuts
remain available: 2/8 for volume and 4/6 for tracks.

In the on-screen keyboard, Center chooses a character, Right soft deletes,
DIAL accepts the text and END cancels. Center also accepts the color
chooser, matching the other settings screens.

The full-player test `tools/rockbox-port/tests/test-idle-power.py --controls`
checks playback pause/resume, actual volume changes, track paths, forward
and backward seeking, and leaving the playing screen through Menu/Back.
It uses normal emulated keypad input and reads player state without
patching guest RAM.
