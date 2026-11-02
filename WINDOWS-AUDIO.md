# Windows InnerAudioContext source implementation

The preview.6 Desktop wrapper installs the native `InnerAudioContext` family
when it is compiled with the official Windows Media Playback C++/WinRT
headers. This adds 36 catalog leaves: the factory, six writable preferences
(`src`, `autoplay`, `loop`, `startTime`, `volume`, `playbackRate`), four read-only
runtime snapshots, five methods, and ten on/off event pairs. `obeyMuteSwitch`
and `referrerPolicy` are deliberately unavailable: Windows has no matching
implemented policy here. This is partial parameter coverage, not full media
or seven-platform acceptance.

## Sources and format boundary

Every nonempty source creates a fresh WinRT `MediaPlayer`, `MediaSource`, and
owned `InMemoryRandomAccessStream`. It never hands a host file URL, network
URL or playlist to the operating system resolver. Inputs must be a strictly
validated, self-contained RIFF/WAVE stream with one 16-byte PCM/IEEE-float
format chunk and one frame-aligned nonempty data chunk. Only `JUNK`/`PAD `
additional chunks are allowed. PCM supports 8/16/24/32 bits; IEEE-float
supports 32/64 bits; both support 1–8 channels and 8–192 kHz. Operating-system
codec/device errors are still returned. MP3, AAC, extensible WAVE, playlists,
remote URLs and other formats require future explicit source-policy work.

- `wxfile://user/...`: read under the managed file operation lock; 32 MiB cap.
- Package-relative paths: require `capabilities.hostServices.apiResourceChunks`
  equal to `1.0`. Only the runtime selects the resource for the exact pending
  source setter. The owner thread passes the complete original call context,
  an offset and a length of at most 1 MiB to `hostApiResource`; native code
  never selects a path or hash. Total length must remain consistent and is
  capped at 16,711,680 bytes. No source reaches the decoder before all chunks
  arrive. Old runtimes explicitly reject this source form.

## Native ownership and observable behavior

A context owns an MTA worker, bounded command queue (32 retained operations),
and bounded native notice queue (64 notices). Each SDK owner has at most
eight contexts. Platform callbacks only enqueue native facts. The runtime
owner thread alone enters the bridge and JS. Ingress overflow faults and
retires the exact owner; it does not drop an arbitrary stream of events while
claiming playback remains healthy.

Replacing `src`, permission reset and backgrounding immediately revoke the
previous source's event and receipt delivery tokens. Previously queued
successful results also become cancellation failures. A fresh source cannot
acquire until old event registrations, player, source, stream and asynchronous
writes are acknowledged closed. The stable public context survives source
reset. Playback in the background is unavailable in this implementation.

`play`/`pause` wait for actual native playback state. `seek` waits for the
native `MediaPlaybackSession.SeekCompleted`; `Seeking` is queued after the
accepted position write and `Seeked` only after that terminal callback. `stop`
waits for native pause and rewind before `Stop`. Failed/cancelled/timed-out
commands drain the decoder before settlement, preserving the context for a
new source. Command cancellation does not claim that a native operation has
already stopped. Native cleanup failures retain exact tokens/handles for
retry. `destroy` and SDK retirement acknowledge only after the worker releases
its native objects. Retirement remains nonblocking with
`ERR_RESOURCE_CLEANUP_PENDING` while cleanup is pending.

Positions and durations convert native 100 ns `TimeSpan` ticks to seconds.
`buffered` uses the officially defined `DownloadProgress × NaturalDuration`
time endpoint, never byte progress. Read-only properties come from actual
native snapshots; a lifecycle transition does not claim paused/reset until
native close has completed. Volume is 0–1, playback rate 0.5–2, and start/seek
times must fit the native nonnegative tick range. `autoplay` and `startTime`
apply during source acquisition. The system media command manager is disabled
so these private players do not take over host-wide transport controls.

`MediaFailed` preserves its original `ExtendedErrorCode`, `ErrorMessage`, and
public `nativeMediaError` enum; activation/operation HRESULTs retain their raw
signed code and permission-denial category. Decode failure leaves the public
context available for another source. Preflight/package/WAVE source failures also emit exactly one object `Error`;
a native `MediaFailed` already reported by the decoder is not duplicated.
Container cancellation/deadline failures do not masquerade as decoder errors,
and cancelling an accepted source revokes its queued events as well as receipts.
Default credentials or fabricated
playback results are never substituted.

## Verification and remaining acceptance

`test/device_audio.cpp` has eight finite groups for WAVE validation/catalog,
exact destroy drain, late receipt fencing, background/reset, delayed native
acquisition/raw failure, bounded commands/backpressure/retirement, and exact
package chunk context/negotiation/progress, plus exactly-once source Error events and cancelled-source fencing. Its controlled backend verifies
ownership and scheduling, not an operating-system decoder or actual JS.

`test/native-sources` additionally compiles every Windows wrapper translation
unit plus explicit MediaPlayer instantiation against the runner's official
Windows SDK. This source-only target does not link a private runtime archive.
Both finite audio and sensor groups run on Windows. The workflow records
compiler and SDK identities and uploads CMake/CTest logs.

Real audio output, supported hardware, rapid native event timing, denial and
background behavior on a physical Windows host, actual QuickJS/V8 facade and
SDK/native transport, full runtime linking, and package-resource FFI integration
remain separate pending acceptance. No mock, source inventory, or green source
compilation replaces those checks. Historical `ORIGIN.json`, artifact locks
and device receipts remain bound to their original versions.

Official semantic references are pinned to
`MicrosoftDocs/winrt-api@4c1e9164521785f82dc63ab57a5ee4644f9d1888`:
`windows.media.playback/mediaplayer.md`,
`mediaplaybacksession_seekcompleted.md`, and
`mediaplaybacksession_downloadprogress.md`. The Windows runner supplies the
actual official C++/WinRT ABI used for checked event removal.
