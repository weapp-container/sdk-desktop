# SDK contract v1

SDK version: `0.1.0-preview.3`; C ABI version: `1`.

Platform adapters call the separately licensed runtime through `weapp_container_sdk.h`. Application
code uses the platform's typed session and view API. The JSON below is an internal
adapter protocol; applications do not construct runtime bridge envelopes.

## Session and scheduling

Create and operate a session on one thread. Android uses a dedicated HandlerThread;
Apple adapters use the main actor. Never invoke a session from a frame/texture
callback on another thread. Deliver user events after the native command returns.
Schedule the completion pump independently of display callbacks: a hidden view
stops presentation and app timers, while HTTP, host completion, cancellation,
timeouts and persistence continue when the OS allows execution.

Each view attachment is exclusive. Detaching preserves the session. Stop rendering
and detach the native GPU context before loading a replacement or closing. Reattach
after both successful and failed replacement: a failed candidate leaves the previous
runtime intact. Candidate storage, requests and messages remain private until startup
and frame preparation have succeeded.

The host owns the stable `storageKey` and supplies an application data `storageRoot`.
Omitting both deliberately selects memory-only storage. A persistent key has one
writer, including during close until the worker releases its lock. Reloading keeps
the lease. Do not derive a key from session identity or package digest.

## Internal commands

Every result is `{ok:true,value:...}` or `{ok:false,error:{code,message}}`.
Release each C result buffer exactly once. Check the result even when the operation
returns no application value.

| Command | Payload / result |
| --- | --- |
| `load` | `{packPath, route?, options?}`; a verified local pack directory |
| `classifyDevPack` | `{packPath}`; fully verifies without changing the session, returns `{reloadKind:"inPlace"|"restart"}` |
| `applyDevPack` | `{packPath,route?,options?}`; returns `{reloadKind,state}` after candidate acceptance, not GPU presentation |
| `devState` | Read-only `{state,pages:[{id,route,data}]}` inspection |
| `clearDevStorage` | Clears current wx storage; adapter must await `flushStorage` for durable completion |
| `registerHostMethod` | `{name}` before the first load |
| `completeHostCall` | `{callId,result}` or `{callId,error:{code,message,retryable}}` |
| `sendMessage` | `{message}` containing JSON data |
| `navigate` | `{route,query?,replace?}` |
| `back` | `{}` |
| `lifecycle` | `{event:"show"}` or `{event:"hide"}` |
| `input` | `{events:[...]}` in logical viewport coordinates |
| `resize` | `{width,height}` in logical pixels |
| `windowMetrics` | `HostWindowMetrics`, including actual pixel ratio and safe area |
| `presented` | `{generation,revision}` captured before drawing; acknowledges that exact native GPU frame after successful host presentation |
| `pump` | `{}` uses the SDK clock; adapters must not substitute uptime for Unix nanoseconds |
| `state` | Current generation, route, viewport, foreground, surface, `frameRevision`, `presentationRequired` and `inputEnabled` |
| `capabilities` | Build features, actual GPU presentation state, APIs and unsupported operations |
| `takeEvents` | Drains the bounded event queue; call after commands and pump |
| `flushStorage` | Returns `{pending:true,ticket}` or `{pending:false}` |
| `close` | Begins async shutdown; returns `{pending:true,ticket}` or `{pending:false,status:"closed"}` |

Observe `ready`, `navigation`, `message`, `error` and `closed` in public APIs.
Adapters also consume `frame`, `hostCall`, `hostCallCancelled` and `storageFlushed`.
Resolve flush tickets only on their matching `storageFlushed` event; resolve close
only on `closed`, propagating its persistence error. Continue pumping while closing.
After completion, destroy the C handle on its owner thread. The synchronous C close
function is a final cleanup barrier, not the UI-facing asynchronous close API.

Host-call identifiers already contain generation identity. They are opaque. Complete
at most once; release host jobs on cancellation. Late/duplicate native completions
cannot run JavaScript in a replacement runtime. Never call FFI inside its own event
dispatch; marshal completion back to the session queue.

## Development reload (preview.3)

Require `capabilities.pack.devReload.protocolVersion == "1.0"` and the selected
reload kind before sending a development command. Older runtimes reject unknown
commands with `ERR_UNSUPPORTED_CAPABILITY`; do not emulate success. Dev Player
creates a session with `gpuRequired:true`: a runtime without the compiled native
GPU provider rejects creation. Omitted `gpuRequired` preserves existing SDK
behavior. This setting never proves that a frame has been presented.

`devDiagnostics:true` explicitly enables bounded development console diagnostics.
It defaults to false and does not change ordinary production session traces.
Adapters forward diagnostic events through their existing owner-thread event stream.

Classification is shared with the simulator. A fully validated WXML/WXSS/static
asset edit can preserve the live session, page data, page stack, storage and
timers. Candidate view/layout preparation runs before acceptance. JS/module,
configuration, dependency, component graph, or unsafe component/WXS changes
restart the session. A restart preserves persistent storage and attempts the
current route if still available. Detach the GPU surface for a restart; do not
detach it for an in-place edit. The adapter serializes classify/apply on the
runtime owner thread and reattaches after either restart success or failure.

Pack validation and pre-acceptance startup failure leave the accepted pack
available. Once a new runtime/frame has been accepted, subsequent GPU failure
is a presentation failure: pause input and retry or explicitly reload the
previous pack. Reloading the previous pack cannot restore its old JS heap.
`clearDevStorage` clears wx storage, not downloaded packs or current page data.

The optional `weapp_container_sdk_gl.h` extension supplies a host-owned GL
function resolver for desktop OpenGL 3.3. The original GLES ABI header is
unchanged. Resolve functions on the current owning context; never share that
context or invoke FFI from fence/completion callbacks.

## Presentation

Use the SDK `frameRevision`, not application frame IDs, to detect redraws: scroll
repaints may intentionally keep an application frame ID. A commit prepares validated
native drawing resources without a CPU page readback. GLES and Metal draw directly
to host targets. CPU presenters acquire an immutable reference-counted RGBA8 frame;
pixels are premultiplied, physical size is logical viewport times pixel ratio.

When `state.presentationRequired` is true, preparing a frame or successfully
calling the GLES/Metal draw entrypoint does not present it. Read `generation` and
`frameRevision` after surface attachment and before drawing; call `presented`
with that captured identity only after a successful EGL swap, completed Metal
drawable presentation, or the equivalent trusted host presentation receipt.
Run the acknowledgement on the session owner thread, never in a GPU callback.
The command returns current state and can release page `onReady` effects that
accept a newer frame. Schedule that newer revision separately; never relabel an
older receipt with the latest state. Late receipts after resize, reload, detach
or reattachment fail with `ERR_CANCELLED` and do not poison the current session.
Each attachment prepares a fresh revision, including reattachment after a
failed replacement, so receipts from the old surface cannot enable its successor.

`state.inputEnabled` identifies whether the current accepted frame has passed
the runtime presentation boundary. Input for an unpresented native GPU frame is
rejected. SDK `ready` reports a committed pack; it is not proof that a host surface
has presented it. When `presentationRequired` is false, the runtime uses its
synchronous raster boundary; CPU platform presenters still track successful
native paints separately and must not claim a GPU attachment or receipt.

GLES attach/present/detach require the owner's current context. Context loss uses
abandon. SDK cleanup must not terminate a display shared with host rendering.
Metal targets must be same-device, BGRA8Unorm, 2D, sample-count one, render-target
usage, non-framebuffer-only and exactly the declared dimensions. Retain textures
until GPU completion. Flutter publishes a completed IOSurface/CVPixelBuffer using a
completion barrier on the same queue; texture callbacks only retain completed frames.

Frame handles outlive sessions and may cross threads with independent retained
references. Frame ownership never crosses Dart or React Native JSON channels.

## Mini-program API

Include `types/weapp-container.d.ts`. `weappContainer.invoke()` returns a Promise;
`postMessage`, `onMessage` and `offMessage` provide bidirectional JSON messages.
`wx.request` performs real asynchronous HTTP(S), supports JSON/text/ArrayBuffer,
timeout and abort. Upload, download and WebSocket are not preview capabilities.
