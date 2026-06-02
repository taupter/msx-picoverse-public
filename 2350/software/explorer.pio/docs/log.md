# Change Log

## v2.30

- Bumped Explorer to v2.30.
- Enabled the primary PSG DAC mirror for Sunrise SYSTEM ROM launches through a reusable SYSTEM audio service, including Sunrise + Mapper modes without stealing mapper I/O or blocking mapper bootstrap.
- Enabled the Dual PSG audio profile for Sunrise SYSTEM ROMs through the SYSTEM audio service.
- Split Dual PSG and primary PSG mirror output into separate stereo channels when both are enabled.
- Kept Sunrise SYSTEM PSG I/O serviced while producing audio buffers so non-mapper Sunrise launches remain stable with Dual PSG plus PSG mirror enabled.

## v2.29

- Bumped Explorer to v2.29.
- Applied each decoded MP3 frame's sample rate to the I2S output clock so MP3 files encoded at rates other than 44.1 kHz play at the correct pitch and speed.
- Retuned the existing MP3 I2S producer format in place on sample-rate changes instead of allocating a second audio pool during playback.
- Expanded Explorer ROM/MP3 record names to the 80-column detail-screen width and only truncate detail-screen names when they exceed the active screen width.
- Reported a missing microSD card to the MSX menu, display a clear no-card message when switching to the microSD source without a card inserted, and avoid reading stale records while keeping the MSX ROM code below the Pico communication window.
- Enabled SDCC code-size optimization for the MSX menu build so the ROM code stays clear of the `0xB900` Pico page-buffer window.
- Show the missing-microSD state as a single menu entry so the user can still switch to Flash or File Hunter from the microSD page.
- Reduced the MSX menu ROM code size by reusing shared row rendering, status text selection, last-line blinking, and source-switch helpers.
- Block File Hunter offline/status message rows from opening the detail screen and fail File Hunter network checks quickly when the ESP8266 does not respond.
- Align the missing-microSD row with File Hunter status rows so it is visible but not selected, scrolled, or actionable.
- Shorten 40-column missing-microSD and File Hunter offline messages so they fit without truncation.
- Refresh status-row text after column toggles so 80-column missing-microSD/File Hunter messages are not reused in 40-column mode.
- Bound each packed Explorer page name so all 19 microSD rows receive a visible name when long filenames fill the page-buffer string pool.
- Persist each ROM detail screen's audio profile and primary PSG setting to a `.PVC` file on microSD, reusing it on later launches and keeping those files out of the menu listing.
- Added the selected mapper to persisted `.PVC` ROM settings while keeping existing audio/PSG option files readable.
- Updated File Hunter catalog requests to use `http://msxpico.file-hunter.com/picoverse.php` while preserving the packed-list `base=1BA0` ROM query parameters required by the Explorer parser.
- Expanded File Hunter page records and detail rendering so 80-column ROM detail screens can show names up to the shared 71-character limit.


## v2.28

- Bumped Explorer to v2.28.
- Added microSD MP3 discovery, MP3 list/detail UI, play counter/status display, full filename rendering, and Play/Stop plus Pause/Resume controls backed by the Pico MP3 decoder service.
- Kept MP3 startup lazy so idle Stop/Pause/Resume commands do not start Core 1 during Explorer startup or normal flash browsing.
- Moved MP3 stream buffering to a dedicated 64 KB PSRAM allocation and reduced the SRAM fallback buffer to 8 KB, freeing SRAM for MSX-MUSIC initialization after MP3 playback.
- Kept the MP3/I2S backend on PIO1 SM2 and preserved the live MSX bus PIO programs when selecting, stopping, or leaving MP3 playback.
- Blocked File Hunter downloads above the 4 MB microSD/PSRAM launch limit used for SD-loaded ROMs.
- Kept the I2S DAC mute line asserted while Explorer is idle, only unmuting it after MP3 playback or a ROM audio profile starts.
- Added a ROM detail PSG option that mirrors primary PSG writes to the Pico DAC and mixes with existing ROM audio profiles.
- Updated Explorer, public, feature, Dual PSG, and PIO documentation for the MP3 player and primary PSG DAC mirroring behavior.
- Reused the MP3 I2S pipeline for ROM audio profiles after MP3 playback, resetting Core 1 before ROM launch and forcing the Pico audio singleton through disable/enable so SCC and MSX-MUSIC DMA output can restart cleanly.
- Moved MSX-MUSIC/FM-PAC I/O bus responders to PIO2, keeping FM-PAC ports isolated from the PIO1 I2S audio backend used by MP3 and ROM audio.
- Started MSX-MUSIC audio output only after the final FM-PAC ROM bus initialization releases `/WAIT`.
- Added temporary USB CDC diagnostics, with TinyUSB host disabled and a CDC-only PicoVerse debug product descriptor, to capture MP3-to-ROM audio launch checkpoints without using UART.

## v2.27

- Bumped Explorer to v2.27.
- Raised the MSX-MUSIC/FM-PAC audio output gain to match the existing SCC, SCC+, and Dual PSG I2S volume boost while preserving sample clipping protection.

## v2.26

- Bumped Explorer to v2.26.
- Added an MSX-MUSIC audio profile to the MSX Explorer ROM detail screen for non-system, non-SCC-class ROMs.
- Ported the LoadROM YM2413/emu2413 audio engine into Explorer and added runtime FM-PAC expanded-slot handling that reads the bundled FM-PAC BIOS from the Explorer UF2 flash payload.
- Moved the Explorer ROM cache from RP2350 SRAM to PSRAM so MSX-MUSIC can coexist with the existing Explorer features without overflowing firmware RAM.
- Updated the Explorer user documentation for MSX-MUSIC profile selection, mapper restrictions, and flash-payload FM-PAC BIOS storage.
- Allowed SPACE as well as ENTER to execute a ROM from the ROM detail screen when `Action: Run` is selected.
- Changed the File Hunter ROM download flow to show `Downloading...` instead of the generic network status check message.
- Removed the WiFi setup hint and F4 special handling from the help page return prompt.
- Increased the Explorer microSD ROM size limit and PSRAM SD ROM buffer from 2 MB to 4 MB.

## v2.25

- Bumped Explorer to v2.25.
- Reworked ROM audio selection around named, mutually exclusive audio profiles so new audio chips can be added without conflicting with SCC or system ROM options.
- Added a Dual PSG audio option for non-system, non-Konami SCC/Manbow2 ROMs, using the same secondary PSG port model as LoadROM.
- Updated the Explorer and PicoVerse 2350 documentation to describe Dual PSG support, audio profile exclusivity, and mapper restrictions.
- Added the shared PicoVerse 2350 Dual PSG implementation reference covering Explorer and LoadROM behavior.
- Updated the public README with user-facing Explorer instructions for selecting the Dual PSG audio profile on supported ROMs.
