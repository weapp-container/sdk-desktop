<a id="windows-bluetooth-le-宿主后端"></a>
# Windows Bluetooth LE host backend

[简体中文](windows-bluetooth.md) · **English**


Status: **pending acceptance**. `src/device_bluetooth_winrt.hpp` contains the real
Windows Bluetooth LE central implementation. The SDK signature review in
`windows-bluetooth-sdk-review.json` used 82 headers extracted from the official
`Microsoft.Windows.SDK.CPP@10.0.26100.9457` package (Windows SDK 10.0.26100.0,
C++/WinRT 2.0.250303.5). This review is source evidence, not a Windows build or
hardware run. Both are still required.

The backend registers 15 async APIs and 5 on/off listener pairs: adapter
open/close/state, discovery start/stop/list, connected-device list, connection
create/close, service/characteristic enumeration, characteristic read/write,
notification/indication configuration, and actual negotiated MTU. It does not
register pairing, peripheral/advertising, MTU setting, or connected RSSI methods.
Those methods remain unsupported by this installed backend.

Creation starts `FromBluetoothAddressAsync` on the attached foreground HWND's
UI thread. Only devices actually discovered by this native owner are accepted;
the native public/random address type is reused. The worker acquires a real
GattSession, enables MaintainConnection, and performs uncached primary service
discovery before acknowledging a connected state. Duplicate service or
characteristic UUIDs are rejected because the public API cannot distinguish
their attribute handles. No synthetic device or success result is returned.

Every native async operation retains its exact handle through terminal status,
GetResults, and successful IAsyncInfo.Close. A late device/session acquisition
is stored before cancellation checks and then closed. A complete native service
result is retained before UUID indexing, including services not yet visited
when an exception occurs. Notification map slots and delivery gates are reserved
before native event registration. Failed or canceled CCCD setup closes the
owned connection rather than leaving a subscription active.

Windows callbacks only enqueue bounded native events. Discovery and notification
generations reject stale ingress, and service changes/disconnection invalidate
queued connection and notification events. Stop waits for the exact watcher's
Stopped/Aborted state. It never stops other applications' radio discovery.
Close invalidates ingress first and retries exact failed unregister/Close steps;
failed cleanup keeps the owner blocked. Event removal uses the generated
interface ABI because C++/WinRT's noexcept event-removal projection discards the
native HRESULT. Successful release steps are consumed once.

Resource bounds are explicit: 32 simultaneous owned connections, 256 discovered
devices, 256 primary services and 256 characteristics per connection. Native
attribute writes are limited to 512 bytes; writes without response additionally
respect `MaxPduSize - 3`. The Windows-managed negotiated MTU is reported unchanged.
`interval`/`powerLevel` scan overrides are rejected. A requested notification or
indication type must be supported by the actual characteristic.

The Windows build requires official C++/WinRT projections with coroutine support,
a foreground native window, an available enabled BLE adapter, and the host's
app/package Bluetooth permissions. The current macOS workspace has no Windows
compiler, OS, or physical peripheral. Windows compile/link, system consent,
physical operations, radio/service changes, late callbacks, backpressure,
background and account-change cleanup therefore remain pending acceptance.
