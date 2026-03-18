# MSX PicoVerse 2040 MIDI-PAC

## Overview

The MIDI-PAC firmware turns the PicoVerse 2040 cartridge into a passive PSG-to-MIDI converter for MSX systems.

It observes AY-3-8910 or YM2149 PSG writes on the MSX bus, maintains a live shadow of the PSG register set, and emits equivalent MIDI events to a USB MIDI device connected to the Pico's USB host port.

The implementation is optimized for real-time operation on RP2040 and is currently tuned for good musical playback on Roland Sound Canvas class modules such as the SC-55.

This document describes the current implementation, the architecture, the transport model, the speed control that was added to stabilize external MIDI devices, the major correctness fixes, the quality-tuning layer, and the remaining limitations.

## Recent Quality Improvements (March 2026)

**State-Driven Frame Architecture**: The PSG-to-MIDI conversion was completely rewritten from a per-register-write model to a coherent 50 Hz frame-based model. A seqlock mechanism ensures atomic PSG register snapshots between Core 0 (IRQ writer) and Core 1 (frame reader), eliminating glitches from partially-updated register pairs.

**Separated Note and Bend Processing**: Note on/off decisions, volume, and mixer state are processed at 50 Hz. Pitch bend updates run at a separate 200 Hz rate for smooth vibrato without excessive note retriggers.

**Stable Note-Center Detection**: Tone channels keep a short recent pitch history and require a new note center to remain stable for multiple frames before retriggering. Small alternating pitch motion stays on the current MIDI note and is rendered as pitch bend.

**Vibrato as Pitch Bend**: Frequency changes within a small window around the active note are expressed as pitch bend rather than note retrigger, which improves Compile-style modulation loops and PSG vibrato effects common in games like Zanac and Nemesis.

**SFX Jump Detection**: When the PSG period jumps by more than ±2 semitones between frames, the converter immediately re-anchors to a new note instead of attempting to pitch-bend through the transition. This prevents wrong-sounding glides during rapid frequency sweeps, arpeggios, and sound effects.

**Same-Note Retrigger Blocking**: If a note-on would produce the same MIDI note already playing, the retrigger is suppressed. This eliminates redundant attack transients that made sustained notes sound choppy.

**Unified Square Lead Voicing**: All three tone channels use Square Lead (GM #81), the closest General MIDI instrument to the PSG's actual square wave output. More aggressive instrument-classification experiments were removed because they degraded game music accuracy.

**Enhanced Noise-to-Percussion Mapping**: Noise frequencies map to 6 frequency bands with musically appropriate GM drum notes (hi-hats, snares, toms, bass drums). Each of the 3 PSG voices gets a distinct percussion instrument per band to avoid all noise voices hitting the same drum.

**Improved Dynamics**: Volume and velocity curves were retuned for better audibility of the three melodic channels relative to channel 10, with stronger low-level presence and a slightly reduced rhythm-channel balance.

**Better SC-55 Compatibility**: Sends GM System On at startup, manages pitch bend sensitivity (±2 semitones via RPN), limits MIDI wire rate to 31,250 baud, and sends All Sound Off after silence detection.

## Goals

The current firmware has three simultaneous goals:

1. Capture PSG activity passively without disturbing the MSX bus.
2. Translate PSG state changes to MIDI with acceptable correctness and low latency.
3. Produce useful musical output on real MIDI modules, especially the SC-55, rather than only proving a technical conversion path.

Those goals are not identical.

Exact PSG emulation would require a full synthesizer or a custom target. General MIDI playback on a module such as the SC-55 requires reinterpretation, especially for PSG noise and amplitude behavior. The current code therefore combines hardware-correct register interpretation with a pragmatic sound-quality layer.

## Source Files

The current implementation is primarily split across these files:

- `2040/software/loadrom.pio/pico/midipac/midipac_main.c`
- `2040/software/loadrom.pio/pico/midipac/midipac.h`
- `2040/software/loadrom.pio/pico/midipac/msx_midipac.pio`
- `2040/software/loadrom.pio/pico/midipac/psg_freq_table.h`
- `2040/software/loadrom.pio/pico/midi/usb_midi_host.h`
- `2040/software/loadrom.pio/pico/midi/usb_midi_host.c`

Their roles are:

### `midipac_main.c`

Main firmware logic.

Contains:

- RP2040 multicore orchestration.
- TX ring buffer and message queue logic.
- MIDI event helpers.
- PSG shadow register interpretation with seqlock-based coherent snapshots.
- 50 Hz frame-based tone and noise processing.
- 200 Hz pitch bend update pass for smooth vibrato.
- SFX jump detection for immediate note re-anchoring.
- Frequency, volume, envelope, mixer, and noise translation.
- Core 1 transmit scheduler and envelope service loop.

### `midipac.h`

Shared definitions.

Contains:

- GPIO pin mapping for the cartridge bus.
- PSG register constants.
- MIDI channel assignments.
- MIDI program defaults for tone and noise channels.
- Frequency table entry structure.
- buffer sizing and conversion limits.

### `msx_midipac.pio`

PIO assembly program for passive PSG write capture.

Contains:

- Logic that waits for `/IORQ` and `/WR` low.
- A GPIO snapshot of address and data lines.
- FIFO push to software.

### `psg_freq_table.h`

A precomputed lookup table mapping PSG tone divisors to:

- nearest MIDI note number.
- MIDI pitch bend value.
- exact fine-note representation.

This table removes expensive floating-point or transcendental math from the real-time path.

### `usb_midi_host.h` and `usb_midi_host.c`

TinyUSB-based USB MIDI host transport layer.

Provides:

- MIDI device enumeration.
- bulk endpoint management.
- raw-MIDI-byte to USB-MIDI packet parsing.
- USB-MIDI packet to raw MIDI byte decoding for RX.
- a small TX staging buffer.
- backpressure reporting so the producer can avoid overrunning the USB side.

## End-to-End Signal Flow

The signal path is:

1. MSX software writes a PSG register index to port `0xA0`.
2. MSX software writes the data value to port `0xA1`.
3. The PIO program observes the write cycle and snapshots GPIO state.
4. Core 0 IRQ code extracts the port and data byte from the PIO RX FIFO word.
5. `midipac_main.c` updates the PSG register shadow.
6. The relevant conversion routine emits one or more raw MIDI bytes into the TX ring.
7. Core 1 drains the TX ring at MIDI wire speed.
8. The USB MIDI host layer packs raw bytes into USB-MIDI event packets.
9. TinyUSB sends the packet buffer to the attached USB MIDI device.
10. The USB MIDI device, cable, or interface forwards the MIDI stream to the target module, such as the SC-55.

## Hardware Architecture

### RP2040 Core Usage

The firmware is explicitly split across both RP2040 cores.

### Core 0 responsibilities

Core 0 owns bus capture and PSG state updates.

Responsibilities:

- configure GPIO for passive input-only operation.
- load and start the PIO write-snooping program.
- handle `PIO1_IRQ_0`.
- latch PSG register numbers.
- apply PSG data writes to the software shadow state.
- translate those state changes into MIDI bytes.

Core 0 is the timing-critical side for MSX bus observation.

### Core 1 responsibilities

Core 1 owns USB host duties and time-based post-processing.

Responsibilities:

- initialize TinyUSB host.
- enumerate and monitor USB MIDI devices.
- send initialization MIDI messages when a device mounts.
- drain the raw TX ring at controlled rate.
- stage outgoing bytes into USB-MIDI packets.
- run the PSG envelope service at a fixed periodic rate.

This split keeps the bus-facing logic separate from the much slower USB host stack.

## Passive Bus Capture

### Design

The implementation is passive.

The Pico never drives the MSX bus for MIDI-PAC operation.

The GPIOs connected to address, data, and control lines are configured as inputs only.

The PIO program waits for an I/O write cycle and captures the entire GPIO state at the moment of the write.

### PIO program behavior

The PIO program in `msx_midipac.pio` does the following:

1. Wait for `/IORQ = 0`.
2. Check whether `/WR = 0`.
3. Delay briefly to let address and data stabilize.
4. Copy all GPIO input bits into the ISR.
5. Push the 32-bit snapshot into the RX FIFO.
6. Wait for `/WR = 1` before accepting another write.

### FIFO word layout

The software interprets the snapshot as:

- bits `7:0`: `A0..A7`, used as the I/O port number.
- bits `15:8`: `A8..A15`, currently ignored for PSG writes.
- bits `23:16`: `D0..D7`, used as the PSG register index or data byte.

For PSG conversion only ports `0xA0` and `0xA1` matter.

## PSG Shadow Register Model

The firmware maintains a 14-register shadow array for the PSG:

- tone frequency low and high bytes for channels A, B, and C.
- noise frequency register.
- mixer register.
- volume registers A, B, and C.
- envelope period low and high bytes.
- envelope shape register.

This allows writes to be interpreted correctly even when the MSX updates related registers in multiple steps.

## PSG Register Interpretation

### Tone frequency registers

Registers `R0-R5` represent the 12-bit tone period for channels A, B, and C.

The firmware combines low and high parts and uses the result as an index into the precomputed frequency table.

The hardware formula is:

$$
f_{tone} = \frac{PSG_{clock}}{16 \times TP}
$$

For the MSX PSG:

$$
PSG_{clock} \approx 1{,}789{,}772.5\ \text{Hz}
$$

Equivalent expression often used in code generation:

$$
f_{tone} = \frac{3{,}579{,}545}{32 \times TP}
$$

### Noise frequency register

Register `R6` is treated as a 5-bit noise period.

The current implementation converts it to a synthetic divisor with:

$$
divisor = R6 \times 8 + 16
$$

That divisor is used during the 50 Hz frame pass to select a GM percussion note from the frequency band table.

This is not a literal physical model of PSG noise. It is a practical mapping that gives the converter a parameter to select musically appropriate drum instruments on MIDI channel 10.

### Mixer register

Register `R7` controls enable and disable state for tone and noise per PSG channel.

The AY mixer semantics are:

- bit clear means enabled.
- bit set means disabled.

The converter mirrors that into six logical MIDI channels:

- tone A, tone B, tone C.
- noise A, noise B, noise C.

Each logical channel is processed independently during the 50 Hz frame pass. The mixer state is read from the coherent PSG snapshot and determines whether tone and/or noise processing runs for each voice.

### Volume registers

Registers `R8-R10` hold:

- 4-bit fixed volume in bits `0-3`.
- envelope enable in bit `4`.

When envelope mode is off, the converter uses the fixed-volume table.

When envelope mode is on, the converter ignores the fixed nibble and uses the periodic envelope service to drive CC#11 expression.

### Envelope period and shape

Registers `R11-R12` form the envelope period.

Register `R13` selects the envelope shape and resets the envelope cycle.

The current implementation follows the AY/YM model of a 5-bit envelope counter with 32 steps per ramp.

## Frequency Conversion

### Why a lookup table is used

Real-time logarithmic conversion from frequency to MIDI note and pitch bend is too expensive to keep in the high-frequency path while also handling bus IRQ load reliably.

The firmware therefore uses a precomputed `psg_freq_table` with 4096 entries, one for each possible 12-bit PSG tone period.

Each entry stores:

- the nearest MIDI note.
- the pitch bend value required to center that note on the exact PSG pitch.
- the exact fine note value scaled by 256, retained for table consistency and offline precision.

### MIDI note mapping

The conceptual formula behind the table is:

$$
note = 69 + 12 \times \log_2\left(\frac{f}{440}\right)
$$

The runtime does not evaluate this formula directly.

### Pitch bend range

The table is built for a pitch bend range of plus or minus 2 semitones.

At initialization the firmware sends RPN `0/0` and Data Entry `2` so the target module uses the same bend sensitivity.

This alignment matters.

If the bend range configured on the MIDI device does not match the bend range assumed by the table, all notes will be detuned.

### Pitch bend ordering

The firmware sends pitch bend before note-on when a note changes.

This prevents the common artifact where a newly-triggered note starts at the previous bend value and glides to the new pitch.

## Volume and Dynamics Model

### Why CC#11 is used

The firmware uses:

- CC#7 for base channel volume during initialization.
- CC#11 for live per-note dynamics.

CC#11 is more appropriate for ongoing musical expression because it can move during a sustained note without implying a permanent channel-wide level change.

### Fixed volume table

The current fixed PSG volume table is deliberately tuned for musical playback on the SC-55 rather than strict electrical AY amplitude matching.

It is defined as 16 entries in `psg_vol_to_midi[]`.

This table was made more aggressive in lower and middle levels so quiet PSG passages remain audible on General MIDI modules.

### Envelope volume table

The envelope table has 32 entries in `env_vol_to_midi[]`.

This matches the 32 hardware envelope steps and was also tuned for stronger low-level detail.

### Dynamic note velocity

The current firmware also derives note-on velocity from current expression.

This is a quality improvement for sample-based MIDI modules.

Without it, all note attacks have the same intensity.

The current heuristic uses:

- a higher base and smaller span for noise channels.
- a lower base and larger span for tone channels.

This lets the SC-55 use velocity-sensitive attack differences while CC#11 still shapes sustained loudness.

## Envelope Implementation

### Service model

Envelope processing is not driven directly by every PSG write.

Instead, Core 1 services the envelope at a fixed periodic rate, currently 50 Hz.

Every 20 ms, `envelope_tick()` advances active envelope counters and updates MIDI expression accordingly.

### Speed computation

The code converts PSG envelope period to a fixed-point step rate using:

$$
step\_rate = \frac{PSG_{clock}}{256 \times EP}
$$

Using the CPU-clock convention adopted in code:

$$
step\_rate = \frac{3{,}579{,}545}{512 \times EP}
$$

For a 50 Hz service tick and a 16.16 fixed-point counter:

$$
speed = \frac{3{,}579{,}545 \times 128}{EP \times 50}
$$

This yields the constant used in the code:

$$
458{,}181{,}760
$$

### Envelope shapes

The current implementation models the AY or YM envelope behavior using a 5-bit counter and explicit logic for the hardware shape groups.

Important points:

- 32 steps per ramp are used.
- shapes that repeat wrap the counter appropriately.
- shapes that hold at full scale use explicit hold behavior.
- writing `R13` resets the active envelope counters.

### What was fixed in the envelope logic

Several critical bugs were fixed during the current development cycle:

- inverted envelope directions.
- wrong step count.
- wrong speed constant.
- broken hold-at-maximum behavior.
- incorrect reuse of fixed-volume conversion logic for envelope values.

Those fixes are fundamental to the current stability and musical result.

## Noise Conversion Strategy

### Why noise is difficult

PSG noise is not naturally representable in General MIDI.

The PSG produces programmable digital noise mixed with tone. A General MIDI module expects pitched notes, controllers, and patch selections. There is no direct standard object corresponding to AY noise.

### Current approach

The current firmware routes noise voices to General MIDI percussion on channel 10.

It does two things:

1. computes a synthetic divisor from the PSG noise register: `divisor = R6 × 8 + 16`.
2. maps that divisor to one of 6 frequency bands, each with 3 drum note variants (one per PSG voice).

Current drum-note families:

- very high noise frequency (div ≤ 24): hi-hat and side stick percussion.
- high noise frequency (div ≤ 48): open hi-hat, ride cymbal.
- mid-high noise frequency (div ≤ 80): snare and clap.
- mid noise frequency (div ≤ 128): toms.
- low noise frequency (div ≤ 176): bass drum and floor tom.
- very low noise frequency (div > 176): deep bass drum.

Each PSG voice selects a different column from the band table, so three simultaneous noise voices produce distinct percussion instruments rather than tripling the same hit.

This is not authentic PSG timbre. It is a pragmatic reinterpretation chosen to sound more useful on an SC-55 than treating noise as a pitched melodic instrument.

## MIDI Channel Model

The firmware uses six MIDI channels.

### Tone channels

- channel 0: PSG tone A.
- channel 1: PSG tone B.
- channel 2: PSG tone C.

### Noise channels

- internal noise A, B, and C shadows are tracked separately.
- when a PSG voice is pure noise, its note output is sent to MIDI channel 10.
- when a PSG voice is tone+noise, the converter keeps the tone path and suppresses the extra drum layer.

This design uses the GM drum channel only for pure-noise cases, which keeps the change conservative and avoids disturbing mixed tone-plus-noise voices.

## SC-55 Oriented Sound Tuning

The current firmware is intentionally tuned to produce a better arrangement-like result on an SC-55.

The initialization logic currently sends:

- Program Change.
- CC#7 Channel Volume.
- CC#10 Pan.
- CC#91 Reverb Send.
- CC#93 Chorus Send.
- CC#11 Expression reset.
- RPN pitch bend range setup.

The default channel voicing currently aims for:

- Square Lead (GM #81) on all three tone channels, faithfully matching the PSG's square-wave character.
- narrow stereo spread across tone channels (pan 60/64/68).
- dry output on the tone channels to avoid masking pitch-conversion errors.
- slightly lower base volume on noise/percussion channels.
- a mix where the three melodic channels stay forward against the rhythm channel.

This is a deliberate quality layer for the Sound Canvas family.

## TX Path and Transport Model

### Raw MIDI byte ring

The converter does not enqueue USB-MIDI packets directly from the IRQ path.

Instead it pushes raw MIDI bytes into a TX ring buffer.

This has several advantages:

- simpler and smaller ISR-side logic.
- no TinyUSB dependency in the bus-facing path.
- ability to reuse the same USB MIDI host parser already used by the standalone MIDI firmware.

### Why the ring size was increased

The TX ring buffer is currently 1024 bytes.

It was increased to provide more headroom during:

- initialization bursts.
- fast PSG passages.
- simultaneous pitch bend plus note-on traffic.
- envelope-driven expression changes.

### Admission control

The helpers `midi_send_2()` and `midi_send_3()` now check that the ring has enough free bytes for the complete message before writing anything.

This prevents partial MIDI messages, which are fatal because a raw MIDI stream is self-synchronizing only at status-byte boundaries.

## Synchronization and Race Control

### Original assumption

The TX ring was originally documented and structured like a single-producer single-consumer queue.

That would have been valid if only Core 0 produced MIDI bytes.

### Actual producer model

The final architecture has two producers:

- Core 0 produces most MIDI bytes from PSG register writes.
- Core 1 produces envelope-driven expression changes.

### Spinlock fix

Because two producers were present, the ring needed message-level atomicity.

Without it, bytes from different messages could interleave, corrupting the outgoing MIDI stream.

The current implementation uses an RP2040 hardware spinlock around `midi_send_2()` and `midi_send_3()`.

This guarantees that one MIDI message is fully written before another producer can write its bytes.

## USB MIDI Host Layer

### Purpose

The USB MIDI host layer converts the raw MIDI byte stream into USB-MIDI event packets and sends them to the attached USB MIDI device.

### TX packet staging

The layer uses a 64-byte aligned USB TX buffer.

Each USB-MIDI event packet is 4 bytes.

The parser collects raw MIDI bytes until a complete MIDI message is formed, then packs it into one USB-MIDI event packet.

### Why backpressure was needed

The 64-byte USB TX buffer can hold only 16 USB-MIDI packets at a time.

If Core 1 fed raw bytes into the parser too aggressively while the USB endpoint was still busy, the staging buffer would overflow and later packets would be silently dropped.

The current host layer exposes `usb_midi_host_can_accept_byte()` so the Core 1 scheduler can stop feeding bytes when the USB side cannot safely accept another packet.

## MIDI Speed Control

### Problem statement

USB MIDI itself can move data in bursts, and a USB MIDI cable or interface can hide the fact that the eventual DIN MIDI output still has to obey standard MIDI timing.

In practice, bursting raw MIDI bytes too quickly caused wrong notes, lost messages, and poor behavior on hardware such as the SC-55.

### Standard MIDI rate

MIDI 1.0 transmits at:

$$
31{,}250\ \text{baud}
$$

Each MIDI byte is framed as 8-N-1, so each transmitted byte actually occupies 10 bits.

Therefore the minimum byte time on the wire is:

$$
\frac{10}{31{,}250} = 320\ \mu s
$$

### Implemented pacing

The firmware defines:

```c
#define MIDI_WIRE_BYTE_US 320
```

Core 1 maintains `next_midi_tx_tick` and only releases another raw MIDI byte into the USB parser when the scheduled time has arrived.

The logic is:

1. call `tuh_task()`.
2. get current time.
3. if a USB MIDI device is mounted and current time has reached `next_midi_tx_tick`, try to send exactly one raw MIDI byte.
4. if a byte was accepted, schedule the next byte `320 us` later.
5. if the USB side cannot accept a byte, do not force more bytes into the parser.
6. call `usb_midi_host_flush()` to submit any staged USB packets.

### Why this matters

This pacing change was one of the key stability fixes.

It prevents USB-side burst behavior from turning into MIDI-device-side corruption or overload.

In practice this significantly improved playback similarity to the original PSG music.

## Initialization Sequence

When a USB MIDI device mounts, Core 1 sends the initial setup state for all six channels.

That includes:

- tone and noise program selection.
- channel volume.
- pan.
- reverb send.
- chorus send.
- expression reset.
- pitch bend range setup using RPN.

This ensures the external MIDI module starts from a predictable known state.

## Important Fixes and Optimizations Applied

This section summarizes the major fixes incorporated into the current firmware state.

### Conversion correctness

- pitch bend sensitivity was corrected to match the lookup table.
- pitch bend is now sent before note-on.
- `R13` correctly resets active envelope counters.
- tone frequency table structure was aligned with the generated table format.
- PSG register reads use seqlock for coherent multi-register snapshots.
- stable note-center detection prevents modulation loops from drifting the base note.
- short centered pitch movement uses pitch bend rather than note retrigger.
- SFX-like pitch jumps (>±2 semitones) re-anchor immediately.
- same-note retriggers are suppressed to avoid redundant attack transients.

### Transport correctness

- raw MIDI transport path was simplified back to the known-good byte-stream model.
- multi-byte message admission control prevents partial writes.
- TX ring size was increased.
- a two-producer race on the TX path was fixed with a hardware spinlock.
- USB staging overflow was mitigated with backpressure.
- Core 1 output was paced to standard MIDI byte timing.

### Sound quality improvements

- conversion rewritten from per-register-write to 50 Hz frame-based processing.
- 200 Hz pitch bend updates provide smooth vibrato.
- dynamic volume moved to CC#11 expression.
- fixed and envelope loudness tables were retuned for better module audibility.
- note-on velocity now follows current expression.
- all three tone channels use Square Lead (GM #81) for faithful PSG character.
- tone channels use a narrow dry stereo image instead of an ambience-heavy mix.
- a 6-band frequency-based noise-to-percussion mapping provides SC-55-friendly drum output.
- melodic channels were retuned to sit more forward than channel 10 in games like Knight Mare.

### Performance optimizations

- runtime expensive pitch calculations were eliminated in favor of a precomputed table.
- bus capture remains lightweight and interrupt-driven.
- MIDI packetization remains in Core 1 and outside the IRQ path.

## Current Limitations

The converter is now substantially better, but it still has important limitations.

### General MIDI cannot reproduce PSG exactly

This is the main limitation.

Sample-based and patch-based MIDI modules are not PSG chips.

Even with correct pitch and timing, the timbre and transient behavior of a square-wave PSG and programmable digital noise generator are fundamentally different from SC-55 instruments.

### Tone and noise coexistence is still simplified

The PSG can mix tone and noise per channel in ways that do not have a clean General MIDI equivalent.

The current six-channel split routes tone to MIDI channels 0–2 and noise to GM percussion channel 10. When both tone and noise are active on the same voice, both paths may emit simultaneously.

### Envelope service rate is still a compromise

The envelope is currently serviced at 50 Hz.

That is stable and efficient, but it does not match hardware-rate envelope stepping.

For some passages, especially fast or percussive material, a faster service rate could further improve smoothness if transport headroom allows.

### Noise mapping is still heuristic

The noise-to-percussion band mapping is musical, not physically accurate.

Different target modules may prefer different drum note choices.

### Module-specific voicing is hand-tuned

The current SC-55 oriented values for volume, pan, reverb, chorus, and noise programs are tuned heuristically by ear and usage goals.

Different users may want:

- stricter PSG authenticity.
- cleaner dry output.
- stronger percussion.
- different stereo image.

## Recommended Tuning Points

If future work continues on MIDI-PAC quality, these are the most useful tuning points.

### Sound quality tuning

- `psg_vol_to_midi[]`
- `env_vol_to_midi[]`
- `tone_pan[]`
- `tone_reverb[]`
- `tone_chorus[]`
- `noise_drum_notes[][]`
- `midi_note_velocity()`
- `MIDI_VIBRATO_BEND_ONLY_DELTA_X256` — vibrato retention threshold
- `TONE_SFX_JUMP_THRESHOLD_X256` — SFX vs. vibrato boundary
- `TONE_STABLE_RETRIGGER_FRAMES` — number of stable frames required before a new note is committed
- `NOISE_MIN_GATE_US` — minimum drum hit duration (currently unused, available for future use)

### Deeper conversion work

- more advanced tone-plus-noise layering strategy.
- higher-resolution envelope scheduling.
- optional module profiles for SC-55, SC-88, software synths, or generic GM.
- adaptive frame rate based on PSG write density.

### Transport work

The current transport is stable enough that future work should avoid radical changes unless they preserve all of the following:

- raw stream message integrity.
- USB staging backpressure.
- 320 microsecond byte pacing.
- dual-producer synchronization.

## Summary

The current MIDI-PAC firmware is no longer a basic proof of concept.

It is now a multi-stage real-time system with:

- passive PIO-based MSX bus capture.
- multicore separation between bus logic and USB host logic.
- a PSG shadow-register interpreter with seqlock coherency.
- 50 Hz frame-based note processing with 200 Hz pitch bend updates.
- SFX jump detection and vibrato-aware pitch bend routing.
- precomputed pitch conversion.
- hardware-correct envelope interpretation.
- a protected raw MIDI TX pipeline.
- USB-side backpressure.
- explicit MIDI wire-speed pacing.
- and an SC-55 quality-optimization layer using Square Lead voicing.

Cristiano Goncalves
2026-03-10