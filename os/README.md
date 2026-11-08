# B310E OS

Our own operating system for the SM-B310E (SC6530C). Design and plan:
[docs/os-design.md](../docs/os-design.md). License: GPLv2 or later
([LICENSE](LICENSE)); the rest of the repository stays MIT. The UI font
data (`ui/font_montserrat.c`, rendered from Montserrat) is under the SIL
Open Font License 1.1 ([ui/fonts/OFL.txt](ui/fonts/OFL.txt)).

## Status

- **Milestone 1** (skeleton): verified on the phone 2026-11-06 (97.3%
  idle at rest).
- **Milestone 2** (storage) and **the first UI**: verified on the phone
  2026-11-07 (15 GB card: self-test, Files, clock, battery, power-off).
- **Milestone 3** (loadable apps), screen sleep, CPU load: verified on
  the phone 2026-11-07.
- **The Rockbox drivers ported** (2026-11-07): audio (codec, speaker /
  headphones, volume, PCM DMA), mic and FM recording, FM radio, headset
  detection and buttons, torch, vibrator, SIM reader, RTC calendar, USB
  mass storage; pass in QEMU. On the phone (2026-11-07): mic recording and
  playback, FM (NOR tables ok), SIM slot 0 and the torch work.
- **USB storage works on the phone** (2026-11-07): a Windows session
  moved 7.7 MB in and 11.7 MB out over 1584 commands, no SD errors. It
  first enumerated but never mounted: failed commands halted the bulk
  endpoints; they now end like the Rockbox port's (a zero-length packet or
  the data taken, then the CSW). The card log lists every command of a
  session with its result and sense (`usb: cmd ...`).
- **The UI** (2026-11-07): an LVGL-like look with less overhead, on the
  phone: Montserrat anti-aliased text, small rounded cards, icons,
  switches, bars, floating dialogs, a warm retro palette.
- **CPU clock scaling** and the corrected CPU accounting: run on the
  phone 2026-11-07: 1.3% load at rest, 0.2% with the screen dark, the
  tick at 1000 interrupts a second, 3913 KB heap free.
- **Battery life** (2026-11-07, in QEMU; not yet on the phone): tickless
  idle (about 50 wake-ups a second on the lit Home screen instead of
  1000, nearly all the END and headset poll; Settings → Deep idle turns
  it off), END polled every 100 ms with the panel asleep, the charger's
  charging / charged lines in the status bar, and a battery line in the
  card log every 10 minutes.
- **Deep sleep, in progress** (2026-11-08). On the phone, dark at 0.1% load,
  the battery fell from 93% to 68% in 10.5 h, about 19 mA from 800 mAh: idle
  is a WFI with the PLL, the crystal and every rail up. The drain log now has
  the time at each clock speed, the holds keeping the clock up and interrupts
  a second. A 4-pole headset no longer keeps the codec, its bias and 104 MHz
  on while the screen is dark and nothing plays (its buttons are off then).
  Tools -> Sleep test tries the chip's sleep levels one by one and logs the
  result (`apps/sleeptest.c`, `arch/sc6530c/sleep.c`); the idle loop does not
  use them until the phone has shown they work. What the stock firmware does:
  - the core stops by 0x205000d0 bit 1; with 0x18 in 0x205000d4 the system may
    stop too. Run from IRAM, interrupts off, no PSRAM command at all;
  - timer 0 (32768 Hz, 0x81000000, IRQ 4) and the 1 ms counter (0x81003000) run
    on; timer 2 (26 MHz, our tick) stops, so time must come from the counter;
  - this board has no 32 kHz crystal. This phone's analog id is 0x1130c000
    (read 2026-11-08), for which stock forces the oscillator on at boot (NOR
    0x1a4f4), so the slow clock has a source; `sleep_chip_init()` does the same
    in the same order, and the test checks the slow clock before going on;
  - at reset the rails' sleep register 0x82001160 holds 0xfb79, which by the
    closest chip's names would cut the flash, the oscillator, the SD card and RF
    in a deep sleep; stock's table (NOR 0xca38c) leaves 0x1340;
  - on the phone: a core stop woken by timer 0 works (1001 ms, 26 MHz ran), and
    so does system sleep after the set-up, every step coming back; in system
    sleep with all clocks open timer 2 still counted the whole second, so
    26 MHz ran. Timing each wake on a slow timer, no step woke late, with the
    clocks closed or the DSP held in reset (0 slow ticks late): the oscillator
    never stops. Stock's sleep path has nothing more; on stock the DSP's own
    program is running, and the status word's DSP bits never show a stop here;
  - the DSP's reset is not held after power-on (the word reads 0), and nothing
    gives its boot ROM a program. The OS now holds it from boot
    (`dsp_hold_reset()`); `dsp_boot()` releases it. Music, FM and recording work
    with it held (phone, 2026-11-08). With the program running (booted, not
    started as layer 1) the oscillator still does not stop, so this phone's
    deepest sleep so far is the system sleep without it;
  - a game with sound left the codec on until power-off: `audio_play_stop()`
    does not power it down and apps cannot. It holds 104 MHz, which is what
    the log showed all night. `audio_idle_tick()` now powers it off after 5 s
    unused, a paused track too, and stock's power set-up runs at boot;
  - stock always boots the DSP; whether a DSP that never ran lets the chip stop
    is not known. The status words in the test's log (0x20500100, 0x8b000120)
    should show it.
- **Status bar**: a symbol for where the sound goes (speaker, headphones, a
  headset with a mic) or a muted speaker: the level of what is playing, with
  nothing playing the music's, or the radio's own mute.
- **More apps** (2026-11-07, in QEMU; not yet on the phone): Calculator,
  Alarm (one daily alarm with snooze; rings while the OS runs), Notes
  (multi-tap text entry), and rename / delete in Files.
- **Optimisations and more apps** (2026-11-07, in QEMU; not yet on the
  phone):
  - Partial LCD updates: only the rows that changed are sent, and nothing
    when nothing changed (idle Home: no transfers instead of one full
    frame a second; a menu move about 45 rows instead of 160).
  - Home is drawn 47 ms after start (it was about 1.4 s): the self-test
    runs behind the UI instead of before it, and the 0.8 s boot pause is
    gone.
  - END and the headset button by the analog EIC's interrupt when it works
    (checked at boot, see the card log's "end key:" line): no polling
    while nothing is held and no headset is in. QEMU does not model it and
    polls. Registers from the stock firmware: `docs/eic-interrupt.md`.
  - Music (Sound → Music, or Center on a .wav or .m3u in Files): a WAV
    player with a playlist, shuffle, playing on in the background; copy
    and paste in Files.
  - Size: LTO saved only 2.6 KB, so the build stays as it is (102 KB
    image; 3.8 MB heap free).
  - System has pages (Left/Right): overview, CPU, threads, storage,
    power; load, clock, interrupts, card I/O, heap and display rows are
    graphs over the last 30 s (`gfx_graph`).
  - Text that ran into other text or out of its box: the DSP screen's
    result, System's rows, Sound's rows; `ui_kv` lays out label/value
    rows so the label gives way. `os/tests/ui-tour.py` shows every screen.
  - The Calculator and Notes are .app modules (in Apps; copy
    `build/os/modules/*.app` to `/b310e-os/apps/` on the card). Apps link
    with `--pic-veneer`: the linker's ARM/Thumb stubs to libgcc loaded an
    absolute address that was never relocated, and the first calculator
    app jumped to 0x125d.
- **USB network and the development shell** (2026-11-07, in QEMU only;
  not yet on the phone): Tools → USB → Network makes the phone a USB
  network adapter (RNDIS, no driver to install on Windows 10/11 or Linux)
  with a small TCP/IP stack of our own, a DHCP server and a shell on
  192.168.7.1 (telnet port 23, raw port 2323): threads, CPU, memory, the
  live log, peek/poke, files, upload and download, and "run" for an app
  built on the PC (live coding). See "USB network and the shell" below.
- **Apps, not screens** (2026-11-07, in QEMU; not yet on the phone): Music,
  Recorder, FM radio and Alarm are .app modules; the OS keeps the services
  under them (the play queue, WAV playing and recording, the alarms and
  their ringing screen), so music, recording and alarms go on whatever is
  open. App API 2.2 reaches the hardware and the services through
  wrappers: threads and sync, the clock, torch, vibrator (timed),
  backlight, keypad light, charge state, raw analog-die registers, volume
  by category, PCM playback and recording, WAV, the playback engine, the
  play queue, album art, the FM radio, alarms, apps by name.
- **Settings** by category, each a page whose rows open an option picker;
  **volume by category** (music, apps, alarm, calls; 0 to 10, each saved,
  heard at once) and **tactile feedback** (a short buzz a key, off to
  strong) instead of one volume and Tools' vibrator test. The status bar
  lost its SD.
- **USB in the background** (Settings -> USB, "Mode": charge
  only, storage or network): the plug starts it with no screen, unplugging
  or a charger with no PC stops it; USB or NET in the status bar. Storage
  sessions run at 208 MHz and log their rate (a Windows mount reads about
  7 MB at some 350 KB/s: the next step is faster packets).
- **Quick panel and notifications** (2026-11-07, in QEMU; not yet on the
  phone): Center held on Home or the Menu opens the quick panel: the
  notifications (newest first, up to three buttons each: Left/Right picks,
  Center presses), then Sound, brightness and the torch. Apps post their own (API 2.4, `os/notify.h`); an app with one up
  stays loaded, so its buttons work with no screen of it open. Music posts
  the track with |< Pause >| and its progress, FM radio the station with
  Mute and Turn off, background USB a Stop. Their icons show in the status
  bar. A short Center press on Home and the Menu acts when it is let go.
- **Sound page and a volume for each app** (2026-11-08, in QEMU; not yet on
  the phone): the panel's Sound row changes what is playing with Left/Right
  (nothing playing: the music) and Center opens the page, with every level:
  music, alarm, calls, tactile, other apps, then one row per app with sound,
  with its icon. Over a game the page opens on the game's row. Settings ->
  Sound & touch -> Each app opens the same page.

  An app needs nothing for this: the first `audio_play` makes a tag under its
  name, at the "Other apps" level, so old builds get one too and an app with
  no sound is not listed. `volume_register("name")` (API 2.10) lists it from
  the start or names it itself; FM radio uses it. A tag works in
  `volume_level`, `volume_set` and `volume_use`, and `VOL_APP` from an app
  means its own. There is still one level at a time: the codec's, for whoever
  last started a sound; there is no mixer. `/b310e-os/volumes.txt` keeps the
  tags ("level icon name" a line, 24 at most, then new apps share "Other
  apps"). LSoft on the page resets: every level back to its default and the
  app rows cleared, but for what is playing; an app comes back when it next
  plays, at the "Other apps" level.
- **The clock** (2026-11-07, in QEMU; not yet on the phone): the phone's
  RTC read 2015-01-01 after some restarts: a reset (the watchdog's, power
  off) clears it, the stock firmware then boots (no key held) and sets its
  default date. Now:
  - **network time**: on the USB network the phone asks the PC by SNTP
    (UDP 123) as soon as it sees it, every 10 s until it answers, then
    hourly; Settings -> Date & time -> Time zone (NTP is UTC) and From
    network. The PC must answer NTP: on Windows the Windows Time service's
    NTP server (off by default), or `python os/tools/b310e-shell.py time`
    sets the phone's clock from the PC's through the shell (`date`);
  - **the clock on the card** (`/b310e-os/clock.txt`, at power-off, before
    a restart, every 10 minutes): a clock behind it at boot was cleared
    and gets the saved time back (the time it was off is lost);
  - the battery reads once at boot on the charger (it showed -1 until
    unplugged).
- **Restarts without a reset** (2026-11-07; B310E OS restart checked in
  QEMU, the boot menu not yet on the phone): sdboot in NOR starts the boot
  menu only with a key held at power-on, so a watchdog reset came back to
  the stock firmware. Restart B310E OS reads `/payloads/b310e-os.bin`
  (until 2026-11-07 also Restart to menu, `/fpbin/fpmain.bin`; now our NOR
  loader takes a request across the watchdog reset, docs/sdboot.md), and the OS
  stops its services, copies the image to 0x04000000 from IRAM with the
  MMU and caches off and enters it (`arch_chain`), as sdboot and the menu
  do. Opening Music the first time draws its screen before reading
  /music, and the tactile buzz ends on time however long a key's work
  takes (a thread of its own).
- **USB storage mount** (2026-11-07, not yet measured on the phone): the
  7.4 MB Windows reads before the drive shows is the whole FAT (16 GB at
  8 KB clusters: 1.94 million entries); its FAT32 driver reads all of it at
  mount. The reads now continue ahead: after a READ's status the next
  16 KB is read while the host prepares its next command, and every 5 s
  of reads the log says where the time went (`usb: msc ...`: reads, card,
  USB and host milliseconds). On the phone: 94% sending at ~210 KB/s, the
  card 6%, the read-ahead hitting 16 of 17 reads; one transfer of many
  packets instead of one per packet was no faster (docs/usb.md, report 17:
  the PC paces the packets). A playing track no longer stops as "taken"
  (a race between the DMA interrupt's fill time and the engine's check,
  about one QEMU run in ten).
- **Fixes from the phone** (2026-11-07, in QEMU; not yet on the phone):
  - The kernel's time could jump 165 s: the timer's countdown can reload
    before its "period ended" bit shows, the time then read one period
    behind, and the unsigned tick count made that 2^32 cycles ahead (a
    playing track then looked "taken", test-codecs failed one run in
    three). The timer now sees a reload by the count going up, and the
    tick count never steps backwards (`arch/sc6530c/timer.c`,
    `kernel/thread.c`).
  - Album art showed only after leaving Now playing and coming back: the
    screen asked for the cover ten times a second and each request
    restarted the decode, so it never finished while the screen was open.
    A request for the cover already asked for now does nothing, the
    worker redraws the screen when it is done (`ui_refresh`, from any
    thread), and covers are read 8 KB at a time on sector boundaries
    (one multi-block card command instead of one per 512 bytes).
  - Yes / No dialogs: any direction moves between the buttons, Center
    picks the selected one (the soft keys still answer directly).
  - The idle load see-sawed between 1.6% and 3.1% on System: a screen's
    tick now falls on the clock's second, so they draw once together; a
    load sample across a wake from deep idle no longer reads 100%.
  - Apps have their own icons in the Apps list (API 2.3: `app_info_t`
    gained `icon`, read from the file; `sdk/icons.h` has the OS's icons
    as initializers).
  - FM radio: Options -> Use speaker plays the radio on the speaker with
    the headset (the antenna) still in; saved with the station.
- **The USB network works with Windows** (on the phone, 2026-11-07): Windows
  binds RNDIS but did not ask our DHCP server and kept a 169.254 address,
  so the phone answers at 169.254.7.1 as well (`net_self` follows the
  address the PC uses); ping 1 ms, telnet and the raw shell work, and
  `tools/b310e-shell.py` tries both addresses.
- **Album art** (2026-11-07, in QEMU; not yet on the phone): Now playing
  shows the track's cover, 60 x 60, from the tags (ID3v2 APIC, FLAC
  PICTURE, MP4 covr, the Vorbis/Opus METADATA_BLOCK_PICTURE comment) or
  beside the file (cover, folder, front, album, AlbumArtSmall, AlbumArt
  .jpg/.jpeg/.png). Our own decoders (`media/art.c`): baseline JPEG with a
  reduced inverse DCT (a 1000 px cover decoded at 1/8, its DC terms only),
  progressive JPEG from its DC scans, PNG with our own inflate; a low
  thread does it, so the music and the UI never wait. `tests/test-art.py`
  checks every source in a screenshot; on the PC, against PIL's own
  downscale, the mean error is 3 to 10 levels in 255.
- **Compressed audio** (2026-11-07, in QEMU; not yet on the phone): a
  playback engine with a decoder thread (`media/`, `include/os/media.h`)
  and Rockbox's decoders as codec modules on the card
  (`/b310e-os/codecs/*.cdc`): MP3/MP2/MP1 (libmad), Ogg Vorbis (Tremor),
  FLAC, Opus, AAC and HE-AAC in .m4a (libfaad), Apple Lossless, WavPack
  and WAV (libpcm: PCM, float, ADPCMs). Music plays them all: playlists
  of every playable file, a now-playing screen (title, artist, album from
  ID3v1/v2, Vorbis comments, MP4 and APE tags; a position bar, elapsed and
  total time), pause, seek, shuffle and repeat. `tests/test-codecs.py`
  plays a tone in every format and checks each in QEMU's output
  (frequency, order, level within 0.5 dB of the WAV one). The image grew
  by 11 KB (119 KB); the codecs stay on the card.
  CPU, in QEMU only (QEMU is not cycle-accurate; the phone is still to
  measure): with `-icount` the decoder thread runs 16.0 M instructions a
  second of audio for a 128 kbit/s 44.1 kHz stereo MP3 (noise and chords,
  so the encoder uses all its bits). At an assumed 1.5 to 2 cycles an
  instruction (ARM926EJ-S from cached PSRAM, no IRAM) that is 24 to 32
  MHz, a quarter to a third of 104 MHz, so the governor should keep 104
  MHz (it goes to 208 MHz above 70%); in QEMU it stayed at 104 MHz, no
  underruns. The 3 s tones (M instructions a second of audio, same
  method): WAV 5.2, FLAC 8.9 (88.2 kHz, resampled: 20), Vorbis 13.5,
  MP3 15, MP2 14, ALAC 17, AAC 20, WavPack 21, Opus 26. On the phone the
  card log has it for every track: `media: <file> done after N ms of
  audio: decoder N ms CPU in N ms (N%), N underruns, N MHz`.
- **Doom as an app** (2026-11-07 in QEMU; runs on the phone 2026-11-08): Chocolate
  Doom 3.1, the boot menu's pinned and patched tree, built as
  `doom.app` (then on fpdoom's tree; now `ports/chocolate`, see "Games"
  below) and opened from Apps like
  any other app: title, demos, music and effects, new game, saves, the
  menu's Quit back to Apps; the app is unloaded and its memory returned.
  App API 2.5 for it: `ui_screen_mode` (a screen that owns the whole
  LCD, gets every key press and release, keeps the screen lit) and
  `ui_call`; apps up to 2 MB (image + bss); an app whose thread ends is
  unloaded at once (before, only when a screen next left the stack).
  The keypad posts a held key's press once: re-enabling the scan after
  each card DMA (`keypad_resume`) latched it again (in QEMU's model since
  it reports keys held at enable), and Doom, reading the card all the
  time, got dozens of presses until the flood cost a release.
- **An app's threads count as the app** (2026-11-07): `malloc`, `free`,
  `app_arg`, `thread_create` and notifications from a thread of an app's
  go by that thread's app (`app_self()` in `loader/loader.c`), no longer
  by `app_current`, the UI thread's (usually no app at all while the UI
  waits). Before, a thread's `malloc` was nobody's and leaked when the
  app closed, and a block taken in the UI thread and freed in the app's
  thread stayed on the app's list and was freed a second time at unload.
  `free` now takes a block off whichever app's list holds it, the lists
  have a lock, and `ui_call` runs its function as the caller's app.
- **Apps in folders of their own** (2026-11-08, API 2.6): an app may sit
  in a folder in `/b310e-os/apps/` with its data
  (`/b310e-os/apps/Doom/doom.app` and the WAD beside it). The Apps list,
  `app_launch` and the shell's `run` look one level into those folders
  (`app_find`), and `app_dir()` tells an app the folder it was loaded
  from. Doom looks for its WAD there first and keeps its settings and
  saves beside it; 7 and 9 quick-save and quick-load.
- **The game kit; Doom, Heretic, Hexen from upstream** (2026-11-08, in
  QEMU): `ports/kit` is what every game app shares (newlib-nano with its
  system calls over the API, the app's thread, screen, keys and sound,
  frames turned and scaled by a view), and `ports/chocolate` builds
  upstream Chocolate Doom, unchanged, as `doom.app`, `heretic.app` and
  `hexen.app` on it. The fpdoom tree, its patch, its libc and its
  assembly are no longer used for them (see "Games" below). * and #
  pressed together open the quick panel on every screen, games too (the
  game is let go of the keys it held); again, it closes.
- **Game Boy and NES apps** (2026-11-08, in QEMU; API 2.7): upstream
  gnuboy and InfoNES on the game kit, with a front end of the kit's: the
  ROMs beside the app, per-ROM options (the screen's turn and scale, the
  keypad, the emulator's own), each game in a fresh app (`app_reload`).
  The kit got a malloc of its own (newlib-nano's realloc could not grow a
  2 MB ROM) and `<dirent.h>`. The UI no longer takes the release of an
  END that went to a game as a press of its own (it went home when the
  game had ended in between).
- **Wolfenstein 3D** (2026-11-08, in QEMU; API 2.8): upstream Wolf4SDL
  on the kit with an SDL and SDL_mixer of our own (see "Wolfenstein 3D"
  below). The app format took unaligned relocations and C++'s static
  constructors; the kit's malloc became best fit (first fit left 524 KB
  free in 30 KB pieces when Wolf4SDL made its sounds) and says in the
  log when it runs out. Tests can show QEMU's screen in a window
  (`B310E_QEMU_DISPLAY`, "Test in QEMU").

| Part | What there is |
|---|---|
| Start-up | identity MMU with cached PSRAM, the high-vector page, I- and D-cache (`arch/sc6530c/cpu.S`) |
| Kernel | preemptive threads, 32 priorities, round robin within one, 1 ms tick (tickless while idle), sleep, semaphores, mutexes with priority inheritance, message queues, heap, CPU clock governor (`kernel/`) |
| Clock | 208, 104 or 26 MHz, chosen by load every 50 ms; 26 MHz only with the panel asleep; Settings → CPU speed: Auto or 208 MHz (`arch/sc6530c/clock.c`, `kernel/cpufreq.c`) |
| Drivers | LCD (ST7735S, panel sleep), backlight and keypad light, vibrator, torch (GPIO7), keypad matrix and END, text console, SD card (multi-block, IRAM bounce, retries), battery voltage and charger, RTC calendar: date and time, set from the UI (`drivers/rtc.c`), SIM reader: activation, ATR, T=0 APDUs, both slots (`drivers/sim.c`) (`drivers/`) |
| Audio | codec power, speaker or headphones by the jack, volume, PCM playback and mono recording on DMA through the VBC, phone mic, 4-pole headset mic, FM line-in; headset detection and its three buttons (KEY_HS_*); all on the ARM, no DSP (`drivers/audio.c`, `pcm.c`, `include/os/audio.h`) |
| USB storage | the card as a USB mass storage device (1209:0001): our own small stack on the Rockbox port's controller driver, all PIO (DMA once wrote stale FIFO words into the card's root directory); Bulk-Only Transport, the SCSI commands Windows and Linux use, two 16 KB buffers; the OS lets go of the card meanwhile (`fs_suspend`) (`drivers/usb.c`, `usb_msc.c`, `apps/usb.c`) |
| USB network | the other USB function (one at a time): an RNDIS network adapter (1209:0001 rev 2.00), MS OS 1.0 descriptors and class E0/01/03 so Windows and Linux bind their own drivers; IPv4, ARP, ICMP echo, UDP, a DHCP server, TCP (`drivers/usb_rndis.c`, `net/ip.c`, `net/tcp.c`); the development shell (`net/shell.c`, `tools/b310e-shell.py`) |
| FM radio | RTC6211N on I2C 1, tune, seek, RSSI, stereo, mute, the stock bank tables read from the phone's own NOR when their CRC matches (`drivers/fm.c`) |
| Storage | FatFs R0.15a on the card: long names, UTF-8, thread-safe; a card log in `/b310e-os/log.txt` (`fs/`) |
| UI | screen stack with status, title and soft-key bars; card lists with icons, switches and details, floating dialogs, text view, a multi-tap text editor (`ui/textedit.c`), key repeat, screen timeout: dim, then LEDs off and panel asleep; drawing with alpha blending, anti-aliased text (Montserrat 9/10/12/24 px) and rounded shapes (`ui/`) |
| Apps | built in: Home (clock, date), Menu (Apps, Files, Tools, System, Settings, Power), Files (rename, delete, copy and paste; music and .m3u open the Music app), Tools (torch, SIM cards, USB: storage or network, DSP boot), System (pages with graphs), Settings (categories: Display, Sound & touch, Power, Date & time, USB, Storage, About; option pickers), Power (`apps/`) |
| Playback engine | a decoder thread below the UI (priority 11) runs a codec module per format and fills a 64K-frame PCM ring (1.49 s at 44.1 kHz) that the audio DMA interrupt plays; play, stop, pause, seek, position, duration, tags; format by extension, or by the first bytes; rates without a codec clock (88.2, 64 kHz ...) resampled linearly to 44.1 or 48 kHz; 16-bit stereo out, no DSP (`media/`, `include/os/media.h`) |
| Album art | the cover from the tags or the folder, decoded small in a low thread: baseline and progressive JPEG (reduced IDCT), PNG (own inflate); 60 x 60 on Now playing (`media/art.c`, `include/os/art.h`) |
| Codec modules | Rockbox's decoders and metadata parsers built unchanged against a shim (`media/rockbox/`) into `.cdc` files with the app layout (`include/os/codec.h`, `media/codecs/`): mpa (MP3/MP2/MP1, libmad), vorbis (Tremor), flac, opus, aac (libfaad, LC and HE), alac, wavpack, wav (libpcm); all fixed point, ARM mode with Rockbox's ARM assembly |
| Loadable apps | `.app` files from `/b310e-os/apps/` (or a folder of their own there, with their data: `app_dir`), relocated into the heap, given a versioned API table, freed when closed (`loader/`, `sdk/`, `include/os/app.h`); opened from Apps or by name with an argument (`app_launch`, `app_arg`), kept loaded while a thread of theirs runs; apps: Music, Recorder, FM radio, Alarm, Calculator, Notes, Stopwatch (`modules/`), Doom, Heretic, Hexen (`ports/chocolate/`, on the game kit `ports/kit/`). API 2.6: the hardware and every service through wrappers (see below), an icon of their own, notifications, full-screen raw-key screens for games, the app's own folder |
| Power | power-off (LEDs off, supplies cut), restart to the boot menu |
| Faults | undefined instruction and aborts show the registers on screen |

On start the console shows the card and a self-test (kernel objects and a
32 KB card write and read), then the UI starts on the home screen.

**Keys:** Center or LSoft opens and selects, RSoft goes back, the D-pad
moves (Up/Down repeat when held, Left/Right page), END goes home, END
held for 2 s powers off, 0 held for 1 s switches the torch (with a short
buzz). The first key after the screen went dark only wakes it. A 4-pole
headset's buttons arrive as KEY_HS_CENTER, KEY_HS_PLUS, KEY_HS_MINUS
(Music, Recorder and FM radio: start/stop or mute, volume).

**On the card** the OS writes only into `/b310e-os/`: `log.txt` (rotated
to `log-old.txt` past 256 KB), `settings.txt`, `alarms.txt`, the album
art cache in `cache/art/` and `selftest.bin` (32 KB, rewritten at every
boot). It loads apps from `/b310e-os/apps/` and codec modules from
`/b310e-os/codecs/`;
recordings go to `/b310e-os/rec/RECnnnn.WAV` (16 kHz mono from the mic,
32 kHz from the radio).

## USB network and the shell

Tools → USB → **Network**, then plug the phone into a PC. The screen
shows the state ("Online" once the PC has enabled the adapter), the
addresses and the frames moved; Back, END or unplugging stops it (and
closes an app the shell started). The card stays mounted.

**Addresses:** the phone is 192.168.7.1; its DHCP server gives the PC
192.168.7.2/24, with no router and no DNS, so the PC's own internet is
left alone.

**Windows 10 and 11:** nothing to install. The adapter appears as
"Remote NDIS Compatible Device" (Device Manager → Network adapters; the
network is "Unidentified", which is fine). Then
`telnet 192.168.7.1` (Windows' telnet client is an optional feature:
`dism /online /Enable-Feature /FeatureName:TelnetClient`), PuTTY (Telnet,
192.168.7.1, port 23) or `python os\tools\b310e-shell.py`. Windows
remembers per VID/PID/revision whether a device has MS OS descriptors;
should the adapter ever bind wrongly after a descriptor change, delete
`HKLM\SYSTEM\CurrentControlSet\Control\usbflags\120900010200` and the
device in Device Manager, then plug in again.

**Linux:** `rndis_host` binds it (an `usb0` or `enx...` interface);
NetworkManager runs DHCP by itself. Without NetworkManager:
`sudo dhclient usb0` (or `sudo ip addr add 192.168.7.2/24 dev usb0; sudo
ip link set usb0 up`). Then `telnet 192.168.7.1`, `nc 192.168.7.1 2323`
or `python3 os/tools/b310e-shell.py`.

**Ports:** 23 is for telnet clients (the phone echoes and edits the line:
Backspace, Ctrl-C, Ctrl-D); 2323 is plain bytes for `nc` and scripts and
the only one that carries binary transfers. Three sessions at a time.

| Command | What it does |
|---|---|
| `help`, `info` | the commands; build, uptime, CPU clock and load, battery, card, network counters |
| `ps`, `top` | every thread: priority (`^` raised by inheritance), state, CPU share over half a second, stack used of its size; `top` repeats until a key |
| `mem` | the memory map (image, heap, frame, MMU table) and the heap: free, largest block, pieces |
| `log [N]`, `log -f` | the last N lines of the card log (kept in RAM, 8 KB), or follow it live until a key |
| `peek ADDR [N] [b\|h\|w] [-f]`, `poke ADDR VAL [b\|h\|w] [-f]` | read or write memory and registers (below) |
| `adi ADDR [VAL]` | an analog-die register through the ADI mailbox |
| `ls`, `cd`, `pwd`, `cat`, `rm`, `mkdir`, `mv`, `df`, `crc` | files on the card (absolute or from the current folder) |
| `put PATH SIZE`, `get PATH`, `shot` | upload, download, the screen as RGB565 (raw port; the helper does these) |
| `run APP`, `close` | close the app that is open, load and start APP (a path, or a name or file name in `/b310e-os/apps/` or its folders); close it |
| `key NAME...` | press keys (up down left right center lsoft rsoft end dial 0-9 * #) |
| `reboot` | restart to the boot menu |
| `echo`, `exit` | |

**peek and poke** are allowed without `-f` on: the PSRAM (read and
write), IRAM 0x40000000-0x4000ffff, the NOR (read only) and the register
blocks the OS keeps clocked: DMA 0x20100000, AHB control 0x20500000,
interrupt controller 0x80000000, timers 0x81000000, ADI master
0x82000000, GPIO 0x8a000000-0x8a002fff, APB control 0x8b000000 (4 KB
each), and USB 0x20300000 while USB runs. A data abort (an address the
MMU does not map, such as 0xfff00000) is caught and reported
(`arch/sc6530c/probe.S` and the abort vector in `cpu.S`). A block whose
clock is off does not abort but stalls the bus, which no handler can
catch: the phone hangs until the battery is pulled. That is why anything
else needs `-f`.

**The helper** (`os/tools/b310e-shell.py`, standard library only; the
address is `--host` or `B310E_HOST`):

```
python os/tools/b310e-shell.py                      interactive
python os/tools/b310e-shell.py cmd ps               one command
python os/tools/b310e-shell.py put notes.txt /notes.txt
python os/tools/b310e-shell.py get /b310e-os/log.txt
python os/tools/b310e-shell.py run build/os/modules/stopwatch.app --follow
python os/tools/b310e-shell.py shot screen.png
python os/tools/b310e-shell.py log
```

**Live coding:** edit an app in `os/modules/<name>/`, `make -C os`, then
`b310e-shell.py run build/os/modules/<name>.app --follow`: the app is
uploaded to `/b310e-os/apps/`, the open app closed, the new one started
on the phone, and its `klog` lines appear on the PC.

How it is built: `docs/usb.md`, "B310E OS: the USB network".

## Writing an app

One folder in `modules/` per app; every `.c` in it is built and linked
with `sdk/start.c` into `build/os/modules/<folder>.app` (staged as
`sdcard/b310e-os/apps/`):

```c
#include "app.h"

static void draw(screen_t *s)
{
    gfx_text_centered(UI_TOP + 40, "Hello", C_TEXT, FONT_LARGE, 2);
}

static screen_t screen = { .title = "Hello", .right = "Close", .draw = draw };

static screen_t *start(void)
{
    return &screen;             /* RSoft pops it; then the app is freed */
}

APP("Hello", start, NULL);
```

The OS names (`gfx_*`, `ui_*`, `list_*`, `f_*`, `snprintf`, `malloc`,
`ticks`, `klog` ...) are macros for the API table (`sdk/app.h`), API 2.0
since the new UI (`gfx_round`, `gfx_icon`, `gfx_bar`, `gfx_switch`,
`gfx_shade`, `gfx_text_height` and the fonts FONT_SMALL, FONT_BODY,
FONT_LARGE, FONT_HUGE; 1.x apps are refused, rebuild them). A screen
with `tick_ms` set gets `tick()` that often (the stopwatch uses 100 ms).
Close your files before your last screen goes; memory from `malloc` is
freed for you, also what a thread of the app's (`thread_create`) took.

## Codec modules

One folder in `media/codecs/` per module: a `module.c` that names the
module, its extensions, its formats and the metadata parser, and a
`codec.mk` that lists the Rockbox sources (relative to `lib/rbcodec` in
`build/rockbox`, the pinned Rockbox clone of the rockbox target; nothing
in it is changed):

```c
#include "glue.h"

static const int formats[] = { AFMT_FLAC, 0 };

CODEC_MODULE("flac", "flac", "FLAC (libffmpegFLAC)", 4 * 1024, formats,
             get_flac_metadata);
```

`make` builds each into `build/os/codecs/<folder>.cdc` (`os.sh` stages
them in `sdcard/b310e-os/codecs/`); without `build/rockbox` it skips them
with a warning. The engine lists the card's modules by their extensions
(read from the files without loading them), so a new module needs no OS
build.

| Module | Formats | Rockbox sources | Image + bss | Arena |
|---|---|---|---|---|
| mpa | .mp3 .mp2 .mp1 | mpa.c, libmad (ARM assembly), mp3 + ID3 parser | 86 + 39 KB | 4 KB |
| vorbis | .ogg .oga | vorbis.c, libtremor, TLSF | 111 + 2 KB | 512 KB |
| opus | .opus (.ogg) | opus.c, libopus (ARMv5E assembly), TLSF | 133 + 28 KB | 256 KB |
| flac | .flac | flac.c, libffmpegFLAC (ARM assembly) | 16 + 214 KB | 4 KB |
| aac | .m4a .m4b .mp4 | aac.c, libfaad (SBR, PS), libm4a | 118 + 82 KB | 512 KB |
| alac | .m4a .m4b .mp4 | alac.c, libalac, libm4a | 24 + 34 KB | 512 KB |
| wavpack | .wv | wavpack.c, libwavpack (ARM assembly) | 22 + 20 KB | 4 KB |
| wav | .wav | wav.c, libpcm | 20 + 44 KB | 4 KB |

## The Apps menu

`/b310e-os/apps` as a list: a `.app` is an app; a folder with an `.app` of its
own name (`Doom/doom.app`, the case does not matter) is an app with its data
beside it; any other folder is a category, opening on a list like this one
(`Emulators/NES/NES.app`). Categories come first, then the apps, each by name;
a name starting with a dot is not listed.

A folder may say more in a text file of its own, `.info`, every line optional:

```
name Retro games
app infones.app
icon 00ffff00ffff
```

`name` is what the list calls it. `app` makes the folder that app, whatever the
file's called. `icon` is 12 such lines, top to bottom, 12 pixels each from 0
(off) to f (full); `python os/sdk/mkicon.py picture.png <folder>/.info` writes
them from a picture and leaves the other lines alone.

## Games: Doom, Heretic, Hexen

`doom.app`, `heretic.app` and `hexen.app` are upstream Chocolate Doom
(3.1, pinned, its source unchanged) built as loadable apps on the game
kit (`ports/kit`, below). Nothing in the OS knows about them: they reach
the LCD, the keys, the card and the sound through the API (2.6), like
Music or the Calculator.

**On the card**: each in a folder of its own, its WAD beside it:

```
/b310e-os/apps/Doom/doom.app      doom.wad (or doom1, doom2, plutonia, tnt,
                                  freedoom1, freedoom2, freedm, chex, hacx)
/b310e-os/apps/Heretic/heretic.app heretic.wad (or heretic1.wad)
/b310e-os/apps/Hexen/hexen.app    hexen.wad
```

Any case. The configuration (`default.cfg`, `heretic.cfg`, `hexen.cfg`)
and the saves go to the WAD's folder. With no WAD beside the app it is
looked for where the boot menu kept it (`/games/doom1`, `/games/doom2`,
`/games/doom`, `/games/heretic`, `/games/hexen`). Without a card or a
WAD the app says so and Back closes it.

**Keys**: hold the phone sideways, the keypad to the right (the game's
top is the LCD's right edge; its 320 x 200 stretched to 160 x 128, each
pixel the mean of the ones it covers: Doom's pixels were never square).

| Key | In the game | In the menus |
|---|---|---|
| D-pad | move and turn, by where it points (Right is ahead) | up, down, left, right |
| Center | fire | select, yes |
| LSoft, 5 | use, open doors | select, yes |
| RSoft | the menu | back, no |
| Dial | run on / off | |
| 1, 3 | strafe left, right | |
| 4, 6 | previous, next weapon | |
| 7 | quick save (the first time: pick a slot; then asks, LSoft: yes) | |
| 9 | quick load (asks; LSoft: yes) | |
| 0 | the map | |
| \*, # | Doom: zoom the map out, in; the view smaller, larger. Heretic, Hexen: the inventory left, right | |
| 2 | Heretic, Hexen: use the artifact | |
| END | quit (asks; LSoft: yes) | |
| \* and # together | the OS's quick panel (volume ...) over the game; again: back | |

Saving and loading are also in the menu (RSoft: Save game, Load game).
In the menus Center, LSoft and 5 are a gamepad's fire button, so an empty
save slot gets the map's name, as upstream does for gamepads. END held
2 s still powers off. The screen does not time out while a game is in
front; alarms still ring over it.

**How they are built** (`ports/chocolate/`, `chocolate.mk`):

- `python os/ports/sources.py chocolate-doom` fetches the pinned upstream
  archive into `build/chocolate-doom` (SHA-256 checked); `make` builds
  the three apps when it is there (`make chocolate` says what is
  missing), and `os.sh` stages each in its folder.
- Upstream's game files build unchanged against newlib. Ours: `i_main.c`
  (main is the kit's), `i_video.c` (frames, keys, timer, no joystick),
  `i_sound.c` (the kit's mixer and MUS/MIDI synth: 8 voices at 22050 Hz),
  `stubs.c` (no network: a game is a local one, and upstream's server
  alone would keep 0.5 MB of buffers; no CD audio, ENDOOM, directory
  listings, text-mode or 640 x 480 start-up), `sdl/` (the few SDL names
  the game files use: types, byte order, qsort; no SDL code), and
  `chocolate_os.c` (each app's OS side: the WAD, the icon). The zone is
  the game's block less 448 KB (upstream asks for whole MiB from 16
  down; the link wraps `I_ZoneBase`).
- Speed: the renderer, the fixed-point maths, the screen's patches, the
  zone and the lumps, the map's geometry, sight lines and monsters are ARM
  code at -O2 (an indexed byte load or FixedMul's smull is one
  instruction; Thumb on the ARM926 has neither), the rest Thumb at -Os.
  FixedDiv is ARM assembly (fpdoom's loop, public domain, with
  upstream's saturation; the link wraps it). Doom's low detail is the
  default (160 columns, each drawn twice: the screen shows 160 anyway),
  in a new `default.cfg` (its Options menu can set it high); Heretic's
  and Hexen's renderers draw only the left half in low detail (the
  originals had none), so they stay high. In QEMU at 250 million instructions a second
  (`tests/bench-chocolate.py`, demo1 of the shareware WAD) that took
  Doom from 97 to 151 frames a second, the demo in step.
- Doom is 433 KB of code and data and 267 KB of bss, Heretic 437 + 258
  KB, Hexen 557 + 300 KB. In QEMU the zone is 2.1 MB for Doom and
  Heretic, 1.9 MB for Hexen.
- `args.txt` beside the WAD adds arguments (upstream's response file):
  `-file sandy.wad -deh sandy.deh`, `-warp 1 1`, `-timedemo demo1` ...
- The log says how long a game took to its first frame and its card
  reads to then ("to the first frame: 177 card reads, 2910 KB"), every
  10 s its frames a second, and the card reads again at the end: the
  phone's numbers.

## The game kit (`ports/kit/`)

What a game or an emulator built as an app shares (`kit.h` is all its
code sees):

- The C library: newlib-nano from the toolchain (BSD), with its system
  calls over the API (`kit_sys.c`): files on the card (relative paths in
  the game's folder; a file opened for reading has no newlib buffer,
  which cut every read into card commands of 1 KB, but a 16 KB cache, one
  card command, so a WAD's lumps that lie one after another, its sprites
  and patches, take one command between them (a card command costs about
  a millisecond on the phone); a file written to has a 4 KB buffer, as
  configurations and saves are written a few bytes a call. newlib-nano's
  fread on an unbuffered file reads a byte a call, so the link wraps it:
  one call into the cache for the whole read. Without that Ultimate
  Doom's start-up was 5 million calls and 26 s on the phone), stdout and stderr to the OS log a line at a time
  (stderr's last line is shown if the game stops with an error), malloc
  on the block the kit takes for the game (`_sbrk`), `exit()` back to the
  kit, the time from the RTC.
- The app (`kit_os.c`): the game in a thread of the app's at `PRIO_LOW`
  (every OS thread first) with the CPU held at 208 MHz; its block of the
  heap (all but 384 KB), taken in that thread, so it is the app's and
  freed with it; a full-screen, raw-key, awake screen (`ui_screen_mode`)
  whose key events the game reads (`kit_key`, the D-pad turned with the
  view); the game ending (or an error, shown with Back) pops it from the
  UI thread (`ui_call`), the screen leaving makes the game save and end.
- The screen: `kit_present8` (palette indexes) and `kit_present565` put a
  frame on the LCD as the view says: turned 0, 90, 180 or 270 degrees,
  fitted (bars), stretched, cropped or 1:1, each pixel the mean of the up
  to 2 x 2 it covers, or the nearest.
- Sound: `kit_audio_start(rate, render)` pulls 16-bit stereo from the
  game in a thread above it into a 4096-frame ring that `audio_play`'s
  fill callback (the DMA interrupt) only copies; a playing track is
  paused. The mixer and the MUS/MIDI synth (`sound/`, first written for
  the boot menu's games) sit on it.
- A game's OS side is a `kit_game_t` (its log name, stack, memory, view,
  and `prepare` for its folder and arguments) and `KIT_APP(name, icon)`.
  An emulator's gives its ROMs' extensions, old folders and options
  instead, and the kit's front end (`kit_menu.c`) does the list, the
  options and the fresh reloads.
- malloc is the kit's own (`kit_malloc.c`) on the game's block:
  newlib-nano's realloc moves a block it cannot grow in place, so a
  buffer grown a little at a time needs twice its size at the end
  (gnuboy reads a ROM 4 KB a realloc: a 2 MB ROM did not fit); here
  blocks grow into the free space after them and free neighbours merge.
  The kit also has the `<dirent.h>` newlib lacks here.

## Emulators: Game Boy, NES

`gnuboy.app` (Game Boy and Game Boy Color: upstream gnuboy, GPLv2) and
`infones.app` (the NES: upstream InfoNES, Apache-2.0; with the GPLv2+ OS
the app is GPLv3 in effect) run on the game kit, their sources pinned and
unchanged (`python os/ports/sources.py gnuboy infones`).

**On the card**: each in a folder of its own, the ROMs beside it (the
boot menu's old folders are looked in too):

```
/b310e-os/apps/Game Boy/Game Boy.app   *.gb, *.gbc   (and /games/gameboy)
/b310e-os/apps/NES/NES.app             *.nes         (and /games/nes)
```

The app opens on its list of ROMs. Center plays one; LSoft opens its
options, kept beside it in `<rom>.opt`: how the screen is turned
(Upright, Sideways, Upside down, Sideways the other way), the scale (Fit,
Fill, Crop, 1:1; the Game Boy starts on Fill: its 160 columns are the
screen's, so text stays sharp across), the pixels (Smooth: no line is
lost; most stay as they are and the few left over are the mean of two, a
Game Boy's going where its 8-line tiles have nothing, so that a font keeps
every line, sharp; Fit near half size across is half exactly and the whole
height, a NES's 128 x 128, so a thin upright line weighs the same wherever a
scroll puts it; Fill, the whole screen, has each column what it covers; Sharp:
the nearest),
whether the
D-pad turns with the screen, and
the emulator's own (Game Boy: the original Game Boy's colours, Blue, Grey
or Green; NES: the 8 lines top and bottom a TV hid, Cropped or Whole, and TV,
Auto, NTSC or PAL: a European game runs a fifth too fast at NTSC's 60 Hz, and
Auto goes by its name and header, which seldom say).
Battery saves go beside the ROM (`.sav`, `.srm`), as do Game Boy save
states (1 saves, 3 loads).

Held sideways the keypad's columns stand as rows would, 7 8 9 to the
right of 4 5 6, as A is to the right of B on a pad:

| Key | Game Boy | NES |
|---|---|---|
| D-pad | D-pad (turned with the screen) | D-pad |
| Center, 7, 8, 9 | A | A |
| 4, 5, 6 | B | B |
| RSoft, Dial, # | Start | Start |
| LSoft, \*, 0 | Select | Select |
| 1, 3 | save state, load state | |
| END | asks to leave: END again, Center or LSoft leave (the battery save written), RSoft plays on | the same |
| \* and # together | the quick panel | the quick panel |

A game always runs in a fresh app: picking a ROM loads the app again with
it (`app_reload`, API 2.7), and leaving loads it again with the list, the
cursor on the ROM, so nothing of one game is left in the emulator's
variables for the next ("One moment..." on the screen meanwhile, not the
screen under the app). Each emulator sets its own pace by its sound: it
waits when it is 2048 frames ahead of what the codec has played. The NES
shows its frames by the clock, nudged to stay with the sound (the codec's
6 ms blocks are too coarse to scroll by), and draws fewer (down to 1 in 4)
when one is late, so the game and its sound keep their speed; every 600
frames the log says how many were drawn and where their time went.

InfoNES is upstream's with the changes `ports/infones/prepare.py` lists and
checks: its sound's envelopes and sweeps never ran (unsigned counters tested
for being below 0, so every effect stayed on its first note) and were not
right once they did (`tests/nes-apu-test.c` holds them to it), a few more of
its bugs, and this phone's speed (the frame RGB565 as it stands, the 6502's
fetches inlined, a line read into the cache before it is drawn).

## Duke Nukem 3D

`Duke 3D.app` is upstream's jfduke3d, jfbuild and jfmact on the game kit, for
`DUKE3D.GRP` beside it or in `/games/duke3d`. Licences: `ports/duke/README.md`.

| Key | In the game | In the menus |
|---|---|---|
| D-pad | move and turn | up, down |
| Center | fire | select |
| LSoft | open, use | yes |
| RSoft | the menu | back, no |
| Dial | run on / off | |
| 5, 8 | jump, crouch | |
| 1, 3 | strafe left, right | |
| 4, 6 | weapon before, after | |
| 7, 9 | item before, after | |
| 0 | use the item | |
| \* | quick kick | |
| # | the map | |
| END | quit (asks; LSoft: yes) | |

**How it is built** (`ports/duke/`): `layer.c` is the engine's platform layer
on the kit, `duke_sound.c` the part of JonoF's audiolib that the game's own `sounds.c`
uses, on the kit's mixer: distance, priorities and loops are upstream's, and sounds
play from the tile cache, no copy. Upstream's tables
come to 5.6 MB, sized for the biggest maps Build ever had; `prepare.py` cuts them to
1.6 MB for what Duke's maps need (its list says what and why), and a movie the tile
cache has no room for is skipped.
`ldiv.S` is a faster 64-bit divide for `divscale` and `scale` (libgcc's took an eighth
of the game's instructions). The card log says every 10 s what the worst frame was made
of: walls, sprites, card reads and the frame's way to the LCD.

## Wolfenstein 3D

`wolf3d.app` is upstream Wolf4SDL (under the GPL, its other licence, id's,
not used) on the game kit, for the registered game's data (`*.WL6`,
GoodTimes/Activision 1.4, Wolf4SDL's default edition):

```
/b310e-os/apps/Wolf3D/wolf3d.app   AUDIOHED.WL6 AUDIOT.WL6 GAMEMAPS.WL6 MAPHEAD.WL6
                                   VGADICT.WL6 VGAGRAPH.WL6 VGAHEAD.WL6 VSWAP.WL6
```

(or in the boot menu's old `/games/wolf3d`). Its configuration
(`config.wl6`) and saves go beside the data.

| Key | In the game | In the menus |
|---|---|---|
| D-pad | move and turn | up, down |
| Center | fire | select |
| LSoft, 5 | open doors | yes |
| RSoft | the menu | back, no |
| Dial | run on / off | |
| 8 (held) | strafe | |
| 1-4 | knife, pistol, machine gun, chain gun | |
| 7, 9 | save, load | |
| END | quit (asks; LSoft: yes) | |

Left and right start at a third of the game's speed and are up to it in
300 ms, so a tap turns a little (`wolf_turn`, the one change to its play
code, in `prepare.py`).

**How it is built** (`ports/wolf4sdl/`): Wolf4SDL is SDL 2 and SDL_mixer
all through, so the port is an SDL of its own over the kit, only what
Wolf4SDL calls (`sdl/SDL.h`, `sdl/SDL_mixer.h`, `sdl_kit.c`): every
surface 8-bit with a palette (the screen too: Wolf4SDL's 8-bit path), the
"texture" the screen goes to is the kit's frame; the keys as scancode
events; a mixer of 8 channels (the digitised sounds, kept 8-bit at their
own 7042 Hz and stepped through as they play: Wolf4SDL keeps every sound
and the whole VSWAP), the OPL music hooked before them, the PC speaker
after. The music and the AdLib effects are DOSBox's GPL OPL emulator
(`USE_GPL`, not MAME's), which makes `id_sd.c` C++, so the game is built
as C++ throughout. The output is 11025 Hz (a quarter of the OPL's 44.1
kHz work). `prepare.py` copies the source to the build with three
changes, checked: Wolf4SDL packs every struct (`#pragma pack(1)` in
`wl_def.h` and `dbopl.cpp`), and an unaligned word load on this ARMv5
does not fault but rotates; the pragmas go and the two structs read from
the map files as they are on disk are packed one by one. Two changes of
the app format came with it (API 2.8): relocations may be unaligned, and
`sdk/app.ld` keeps C++'s static constructors (`.init_array`), which the
kit runs before the game (DBOPL fills its tables in them).

## Build

```
build.ps1 os        (Windows)
bash build.sh os    (Linux)
```

Output: `build/os/b310e-os.bin`, staged as `sdcard/payloads/b310e-os.bin`.
The boot menu lists it under payloads. The first build downloads FatFs
R0.15a from elm-chan.org into `build/fatfs/` (checked against its
SHA-256); the OS uses its own `fs/ffconf.h`.

## Test in QEMU

To watch a test, set `B310E_QEMU_DISPLAY` to a QEMU display (`gtk`,
`sdl`; `gtk,zoom-to-fit=on` lets the 128 x 160 screen be made larger):
the tests run as they do without it, QEMU's window shows the screen as
it goes (`$env:B310E_QEMU_DISPLAY = "gtk,zoom-to-fit=on"` in PowerShell,
`B310E_QEMU_DISPLAY=gtk python os/tests/test-doom.py ...` in bash). The
keys the test sends still go to QEMU; ones typed in the window too.

`B310E_QEMU_AUDIO` adds QEMU's sound to the game and app tests, so you can
hear them too: `export B310E_QEMU_AUDIO="-audiodev sdl,id=audio0,in.voices=0
-global sc6530_adi.audiodev=audio0"`.

```
python os/tests/test-boot.py
```

Boots the image with a FAT32 card image (with the stopwatch app on it)
and reads the OS's `os_status` block through QTest: self-test results,
card, idle share, then drives the UI with key presses (Menu, Apps and the
stopwatch, Files, System, Settings, END home, screen dim, dark and wake,
Power off) and checks the card image for the self-test file, the saved
settings and the log. Screenshots of every screen go to `build/os/test-boot/`.

```
python os/tests/test-audio.py
python os/tests/test-sim.py
python os/tests/test-rtc.py --build build/os
python os/tests/test-tickless.py
python os/tests/test-sleep.py
python os/tests/test-volume.py
python os/tests/test-apps.py
python os/tests/test-net.py
```

```
python os/tests/test-codecs.py [--icount SHIFT]
python os/tests/test-art.py
python os/tests/test-doom.py --wad path/to/doom1.wad
python os/tests/test-chocolate.py --game heretic --wad path/to/heretic1.wad
python os/tests/test-chocolate.py --game hexen --wad path/to/hexen.wad
python os/tests/test-emu.py --app gnuboy --rom path/to/game.gbc
python os/tests/test-emu.py --app infones --rom path/to/game.nes
python os/tests/test-wolf.py --data path/to/wolf3d
```

`test-codecs.py` makes a 3 s tone at -6 dBFS in every format a module
was built for (ffmpeg: libmp3lame, mp2, flac at 44.1 and 88.2 kHz,
libvorbis, libopus, aac, alac, wavpack, PCM; each at its own frequency,
tagged), plus a 40 s 128 kbit/s stereo MP3 of noise and chords, and
plays the folder in Music: each track by itself after the last, then on
the long MP3 the decoder's CPU share, pause (the position stands),
Right held (seeks), Right tapped (the next track: the first again). In
QEMU's WAV output every tone must be there, at its frequency, in order,
within 1.5 dB of the WAV one; the tags must reach the log. With
`--icount` QEMU counts instructions (one per 2^SHIFT ns), and the test
reports the decoder's instructions per second of audio.

`test-audio.py` plugs in a 4-pole headset, checks its centre button,
plays the 1 kHz test tone (measured in QEMU's WAV output), records the
model's mic tone to the card and checks the WAV there, plays it back and
starts the FM radio (probe, start-up and seek in the tuner model's
trace). `test-sim.py` reads both slots with a card in either and checks
that only SELECT MF reached the card and nothing identifying was logged.
`test-rtc.py` sets dates through the Date & time screen and reads the
RTC registers back. `test-tickless.py` counts the interrupts a second on
the idle Home screen and checks the kernel's time against the RTC model,
then turns Deep idle off and checks that the tick is back. `test-apps.py`
adds an alarm for 00:01 every day in the Alarm app and snoozes it when
the OS rings it, works the calculator, types a note by multi-tap, plays
two tracks in Music (the second by itself; the WAV codec module is on its
card), pauses, keeps playing once the app is closed, opens a track from
Files in Music, and copies and pastes the playing file in Files; the tests read the card's files
through its FAT (`card_file()` in `test-boot.py`), since the log is
fragmented once other files are written. `test-net.py` is the PC on the
USB network: it enumerates (MS OS descriptors included), runs RNDIS,
DHCP, ARP and ping, then a TCP stack of its own against the shell (both
ports, every command group, a 96 KB upload and download, an app uploaded
and run, `log -f` while a second session starts it, a lost segment
retransmitted, the helper script's own calls), unplugs and checks the
files in the card image.

`test-doom.py` puts `doom.app` and a WAD (yours: `--wad`,
`$B310E_DOOM_WAD` or `sdcard/games/doom1/doom1.wad`) in
`/b310e-os/apps/Doom/` on the card and opens
Apps -> Doom (upstream Chocolate Doom on the game kit): the title and its demo (frames counted by the kit, Doom's
colours in a screenshot, sound in QEMU's WAV output), the menu, a new
game on E1M1, a walk (the view moves), a save in slot 0, a walk on and
the save loaded (the view is back), the same with 7 and 9 (quick
save, its question the second time, quick load), END and yes: back in Apps, the app
unloaded, the heap within 16 KB of before (the game's block, taken in
its thread, is on the loader's list of Doom's memory), `default.cfg` and
`doomsav0.dsg` on the card, no `temp.dsg`, "doom: ended (0)" in the log;
on the way, * + # opens the quick panel on Home and over the game (Right
held meanwhile is let go for the game: the view stops).
`test-chocolate.py` does the same for Heretic or Hexen up to a walk in a
new game, then quits and checks the unload, the heap and the log.
`test-emu.py --app gnuboy|infones --rom <rom>` puts the app and the ROM
in its folder: the list, the ROM's options (changed, kept in `.opt`), the
game (frames, a screenshot with its colours, keys), END asking (the game
waits; RSoft plays on), END twice back to the list in a fresh app with
Apps never on the way, Back out of it, the heap back where it was.
`test-duke.py --grp <DUKE3D.GRP>` goes through the logos and menus to a new
game (a map loaded, the view changing as it walks), then quits and checks the
unload, the heap and the log.
`test-wolf.py` puts Wolf3D and its data in its folder: the title, the
menu, a new game, a turn (the view changes), END and yes, the unload,
the heap and the log.
`bench-chocolate.py` runs a game's `-timedemo` in QEMU at a fixed
instruction rate (`-icount`) and prints its frames a second and the
time to its first frame: the same for the same code, for comparing
builds (not the phone's: no cache, no PSRAM waits; QEMU's card costs
real time a command).

## Layout

```
arch/sc6530c/   start-up, MMU, context switch, interrupts, timer, ADI, power
kernel/         threads and scheduler, sync objects, heap
drivers/        LCD, backlight, keypad, console, SD/SDIO, battery, fonts
fs/             FatFs glue (disk, locks, config), card log
loader/         app loader, the API table apps get
net/            the USB network's IPv4/TCP stack and the development shell
media/          playback engine, play queue, album art, codec module
                loader; rockbox/: the shim headers; codecs/: one folder
                per codec module, glue, runtime
ui/             drawing, screen stack, widgets, fonts (OFL), icons, volume, alarms
apps/           built-in screens: home, menu, apps, files, system, settings, power,
                tools, clock (date and time), usb, sim, dspboot, wavio
sdk/            for loadable apps: app.h, app.ld, start.c, mkapp.py
modules/        loadable apps, one folder each
ports/          games as apps: kit/ (the game kit: newlib's system calls, the
                app, frames, keys, sound), chocolate/ (Doom, Heretic, Hexen
                from upstream Chocolate Doom), gnuboy/, infones/ (the Game
                Boy, the NES), wolf4sdl/ (Wolf3D: its SDL of ours),
                sources.py (the pinned sources)
tools/          mkfont.py (Montserrat -> ui/font_montserrat.c), mkicons.py,
                b310e-shell.py (the PC side of the shell)
lib/            string functions, printf
include/os/     interfaces
link/os.ld      memory map
tests/          QEMU tests
main.c          start-up; selftest.c: the boot self-test
```

## Notes for the code

- Every assembly routine needs `.type name, %function`: C is built as
  Thumb, and without the type the linker calls ARM routines with `bl`
  instead of `blx` (the first boot in QEMU ran ARM code as Thumb).
- Threads run in SYS mode; interrupts and `svc` save the same 17-word
  frame on the thread's stack (`cpu.S`).
- The LCD transfer is started, not waited for: the LCDC's done flag does
  not report reliably, and polling it cost half the CPU.
- Partial LCD updates (`drivers/lcd.c`): `lcd_update()` compares the
  back buffer with the frame last sent (the LCDC's source still holds
  it), and sends the band of changed rows through the panel's row window
  (RASET) and the LCDC's LCM crop (`lcm_start`, `lcm_size`), as the stock
  firmware does; the QEMU model follows the crop. A band of most of the
  screen goes whole; the spacing between transfers scales with the rows.
- QEMU on Windows loses about 0.25 ms per timer period (its periodic
  ptimer restarts each period from the late host callback), so with the
  1 ms tick time in QEMU runs some 26% slow; tickless, with about 50
  periods a second, within 1%. The idle share is a ratio of counted cycles
  and stays right. The chip reloads in hardware.
- Tickless idle (`kernel/thread.c`): with only the idle thread ready and
  the next timeout three or more ticks away, the idle thread loads timer 2
  with that many ticks (a second at most) before WFI. Ticks follow the
  timer's cycle total (`timer_cycles()`), so any interrupt first catches
  them up (`kernel_irq_entry()`), then loads the rest of the tick and,
  when that ends, the tick again: the reload repeats the last load. A
  woken thread mostly runs for well under a tick, so the idle thread may
  sleep again from that rest-of-tick period (waiting for it first doubled
  the interrupts: 100 a second instead of 50). The idle thread catches
  the ticks up before it decides; when that wakes a thread, it runs it
  instead of sleeping. It slept on at first, so a thread whose timeout had
  just passed waited for the next timeout instead: the END key's 20 ms
  poll once slept 585 ms, and short END presses were lost (QEMU tests,
  about one press in 30). The first version stalled
  at tick 19: catching up took longer than the rest
  of the tick, and the negative load ran as a 165 s period; the rest is
  signed now, and a late one skips to the next boundary.
- Pixels are plain RGB565. Milestone 1 packed them byte-swapped (copied
  from Rockbox's video helper), which only white survived; the QEMU
  model showed it.
- FatFs reads the FAT one sector at a time, and a single-block read
  costs about 1 ms on the phone, so counting a 15 GB card's free space
  took 15 s at the first boot (its FSINFO had no count). `fs/diskio.c`
  reads the FAT region 32 sectors ahead, and when FSINFO has no count
  the log thread counts after boot; FatFs writes it back to FSINFO.
- The screen timeout first only set the LED to 0 and stopped refreshing;
  on the phone the screen stayed lit and froze (2026-11-07). Dark now
  does what Rockbox does (verified there): LED and keypad light off,
  display off and panel sleep (`lcd_sleep`), and the first key wakes it.
- Dark still left a faint glow on the phone: "off" (from fpdoom) only
  cleared the LED's on bit. The stock on/off function (NOR `0x1a186`)
  clears bits 5 and 6 of `0x82001220`, then sets bit 6 for on or bit 5
  for off; `drivers/backlight.c` does the same now.
- A full frame takes about 10.6 ms on the BOE panel (27 bus clocks a
  write at the 150 ns timing, two writes a pixel), not the 6 ms assumed:
  at 8 and then 10 ms spacing the phone showed half-drawn frames and
  screens that lagged until the next redraw. The spacing is now computed
  from the programmed timing plus a quarter (14 ms), and the boot log
  records the real transfer time by the LCDC's done flag
  (`lcd: transfer spacing ... done flag after ... us`).
- Everything draws into a back buffer that `lcd_update()` copies to the
  LCDC's frame just before a transfer, as Rockbox does. Drawing straight
  into that frame showed dark horizontal lines on the phone where text
  was being redrawn (the stopwatch, 10 times a second).
- The System screen shows the CPU clock and load (100% minus the idle
  thread's share), interrupts per second and the two busiest interrupt
  lines (#23 is the 1 ms timer, #8 the keypad).
- CPU time was measured as the tick counter's difference modulo one
  period, which loses every whole period the idle thread sleeps through:
  the phone showed 10% load and 17000 timer interrupts a second at rest
  (really about 1000). `timer_cycles()` is now a monotonic 26 MHz count
  (periods plus the position in the current one, a pending period
  included).
- With the screen dark the UI loop spun: `redraw()` returns early and
  leaves the frame dirty for the wake, and the loop took "dirty" as "do
  not wait". It waits now; the QEMU test checks 99.9% idle while dark.
- Clock levels keep the AHB at 104 MHz at 104 and 208 MHz, so the LCD
  timing, SDIO and ADI run as at boot; only 26 MHz changes the AHB, and
  SD transfers (`cpufreq_hold`) and the awake panel (`cpufreq_allow_low`)
  keep it at 104 MHz or more. Switch order and register meaning are from
  the stock firmware (`arch/sc6530c/clock.c`). If the boot clock is not
  one of the three levels, scaling stays off. If Auto ever misbehaves on
  the phone, `cpu_max=1` in `/b310e-os/settings.txt` pins 208 MHz.
- RTC (from the stock rtc_phy_v5 driver, NOR 0x35dac..0x363d0, and the
  time manager at 0x19594..0x19b9e): seconds, minutes, hours and a 16-bit
  day counter at 0x82001600..0c, day 0 = 1980-01-01; new values go to
  +10..+1c and the RTC acknowledges each in raw status bits 8-11 (+34,
  cleared at +38). FAT timestamps come from it. The Rockbox port's RTC
  driver read the date as packed fields; it uses the day counter too now
  (docs/rockbox-power.md).
- Headset buttons need the mic bias, which needs the codec powered:
  while a 4-pole headset is in, the codec stays on (Rockbox keeps it on
  from boot). A 3-pole plug turns it off again after the check.
- The charger level (analog EIC channel 2) reads 0 until its mask bit is
  set in 0x82001904, which the OS never did: "charging" could not show.
  `charger_present()` sets it now (found while porting USB, which detects
  the cable through the same line).
- `_start` reads the CP15 control register it is entered with. If the
  loader left the MMU and D-cache on, it writes the cache back and
  switches them off before zeroing `.bss`: otherwise stores to lines the
  loader had cached stay in the cache, `mmu_setup`'s invalidate drops
  them, and those variables keep the loader's data. Confirmed on the
  phone (2026-11-07): the boot menu enters with control register
  0x0005327f, MMU and D-cache on. Before the fix the UI's screen stack,
  key timers and the app list held its data (small values, a frame-buffer
  pointer): a data abort leaving the Stopwatch, then the torch, a buzz and
  a power-off at every boot, and a first frame drawn only after a key
  (the screen state read dark). The boot log records the entry state
  (`boot: entered with control register ...`). Rockbox's crt0 makes the
  same assumption.
- The system DMA engine is quieted first thing at boot as well
  (`drivers/dma.c`: all 32 channels off, all 64 request bindings
  cleared), so nothing the loader set up can run once the audio code
  enables its clock. `tests/test-dma.py` plants such channels first.
- The UI: no object tree and no style system, unlike LVGL. Screens draw
  what they show into the back buffer every frame with a few primitives
  that blend (5-bit alpha in the 0x07e0f81f spread form): Montserrat
  glyphs and 12 px icons as 4-bit alpha maps made at build time by
  `tools/mkfont.py` and `tools/mkicons.py`, rounded rectangles with a
  coverage table per radius. A full frame takes a millisecond or two. The
  look: a warm dark palette (cream text, amber accent and selection, sage
  and brick for good and bad), corners of 2 px, 18 px list rows.
  `tests/ui-tour.py` takes a screenshot of every screen.
- FM seeking tunes one channel per UI tick (about 16 a second) and any
  key stops it. It first swept the band inside one key handler: on the
  phone, Up while recording the radio blocked the UI, the recording and
  END for minutes. As a failsafe the END thread powers off by itself when
  END is held for 8 s (the UI does it at 2 s).
- **END held 2 s on an app** closes the app, not the phone (with no app on the
  screen stack it powers off, as above). Its screens go, which asks it to end, so
  a game saves; after 1.5 s more its threads are ended where they are (a mutex of
  the app's own is let go; a thread holding one of the OS's ends when it lets go
  of that), and it is unloaded as usual. `test-duke.py` hangs a game to check it.
- **A lost key release**: the keypad interrupt cleared every edge after reading them,
  so a release arriving just then was lost and the key's next press was taken for the
  same one (QEMU: 4 keys wrong in 400 with the card busy). It clears only what it read.
  Do not end a held key by the scan's silence after a card transfer: tried on
  2026-11-08, and on the phone it dropped keys still held (Doom's walking, a list's
  scrolling), so the scan does not always report them again.
- The torch pin is set up (off) at boot, as Rockbox's power_init does:
  configured only on the first toggle, that toggle was lost on the phone.
- Log lines written while USB storage has the card are kept and written
  once it is back.
- Audio, SD transfers and the awake panel hold the CPU clock at 104 MHz
  or more (the AHB feeds their timing); SIM sessions do too.
- The playback engine (`media/engine.c`): decoding never runs in the
  DMA interrupt. The interrupt only copies frames out of the ring and,
  when the decoder waits for room and there is enough, posts its
  semaphore; so the decoder works one codec frame at a time as the audio
  drains (decoding the ring full in bursts made the governor jump between
  104 and 208 MHz). Playback starts with 0.3 s buffered; an empty ring
  plays silence and counts an underrun. Pause stops the DMA and keeps the
  ring. Audio holds the clock at 104 MHz or more (`audio_power`); the
  governor goes to 208 MHz only when the load needs it.
- Codec API (`include/os/codec.h`): Rockbox's `struct codec_api` with
  the same member names (the decoders build unchanged), without the
  multi-core, profiling and recording parts, plus file calls for the
  metadata parsers. File data comes through a 64 KB window of the file
  (grown when a codec asks for more), read in the decoder thread.
  `pcmbuf_insert` takes what `configure` said (16-bit, or 32-bit with N
  fraction bits; interleaved, two planes or mono) and rounds and clips it
  to 16-bit stereo: that is the whole "DSP".
- A codec sets its sample depth once, at `CODEC_LOAD`, not per track
  (Rockbox's DSP keeps it): resetting it at each track made every track
  after the first of a module play garbage (int32 samples read as int16).
- Modules are .app-format files with their own magic (`mkapp.py --magic
  --info`), linked by `media/codecs/codec.ld` with `--pic-veneer` (the
  interworking stubs to libgcc's ARM code would otherwise hold absolute
  addresses no relocation fixes). The loader keeps one module; the next
  track of the same format reuses it, an idle one is freed after 30 s.
  For an extension several modules claim (.m4a: AAC and ALAC; .ogg:
  Vorbis and Opus) each one's parser is asked in turn (the file's first
  bytes pick the likely one first); a module refuses a format it does
  not decode.
- The shim (`media/rockbox/`) stands in for Rockbox's config, platform,
  codecs.h, rbunicode (ISO-8859-1 and UTF-16 to UTF-8) and a few C
  library headers; `media/codecs/rt.c` is the module's C runtime (string
  calls, `strtol`, `snprintf` through the engine, file calls),
  `setjmp.S` Tremor's escape when its arena is full. No module calls
  floating point (checked: no `__aeabi_f*`/`__aeabi_d*` in any).
- Music (`modules/music/music.c`, a loadable app) talks to the play
  queue (`media/queue.c`) and the engine only through the app API, so the
  queue goes on playing once the app is closed; the music volume is
  Settings' (`volume_use(VOL_MUSIC)`). Left/Right: a tap skips (decided 0.6 s
  after the press, when no key repeat came: the UI does not pass key
  releases to screens), a hold seeks and plays from the new place when
  the repeats stop.
