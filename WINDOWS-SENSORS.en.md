<a id="windows-传感器"></a>
# Windows sensors

[简体中文](WINDOWS-SENSORS.md) · **English**


`DeviceApis` installs the following 12 APIs when the desktop wrapper was built
with the official C++/WinRT sensor headers. Each start uses the corresponding
real `Windows.Devices.Sensors` class; `GetDefault()` returning null fails the
operation and does not invent sensor readings.

| Sensor | Start / stop | Subscribe / unsubscribe | Native values |
| --- | --- | --- | --- |
| Accelerometer | `wx.startAccelerometer` / `wx.stopAccelerometer` | `wx.onAccelerometerChange` / `wx.offAccelerometerChange` | Portrait axes, acceleration in g |
| Compass | `wx.startCompass` / `wx.stopCompass` | `wx.onCompassChange` / `wx.offCompassChange` | Magnetic north in degrees; native accuracy when known |
| Gyroscope | `wx.startGyroscope` / `wx.stopGyroscope` | `wx.onGyroscopeChange` / `wx.offGyroscopeChange` | Portrait axes, angular velocity converted from degrees/s to radians/s |

`normal`, `ui`, and `game` request 200, 60, and 20 ms intervals, bounded by the
native sensor's `MinimumReportInterval`. These are requested intervals, not a
guarantee about device sampling. Compass exposes no sampling option in the
locked API contract. The implementation does not label true north as magnetic
north and omits unknown accuracy.

Native callbacks only enqueue bounded values. The runtime owner thread drains
them through the existing event channel. A full queue retires the source. Stop,
permission reset, backgrounding, call cancellation before result delivery, and
account-owner retirement revoke already queued values immediately. Stop waits
for all earlier sources of that sensor kind, including owners still draining
failed cleanup. Starting a replacement while old cleanup is pending fails.

The source retains its MTA, native sensor, registration token, and process lease
until `remove_ReadingChanged` returns a checked success. The projected removal
overload is `noexcept`, so the implementation calls the same official SDK ABI
and checks its HRESULT. Failed removal or configuration restoration retries
the exact remaining step. Successful steps are not repeated. The original
report interval and reading transform are restored before releasing the source.
The lease prevents two SDK sessions from changing the same app sensor at once;
hosts using these native classes directly must coordinate that use with the SDK.

These three WinRT classes do not expose `RequestAccessAsync`. An actual
`E_ACCESSDENIED` is returned as permission denial, with the native HRESULT in
`errCode`. The adapter does not fabricate a WeChat authorization scope or an OS
permission prompt.

The finite lifecycle target can run without a private runtime archive:

```sh
cmake -S test/native-sources -B /tmp/weapp-native-sources
cmake --build /tmp/weapp-native-sources --config Release
ctest --test-dir /tmp/weapp-native-sources -C Release --output-on-failure
```

The dedicated Windows workflow also compiles the complete Windows adapter and
all three sensor registration/removal specializations against the installed
official Windows SDK. Its OBJECT target performs no runtime link. Compilation,
controlled-source lifecycle tests, physical sensor acceptance, and packaged
runtime acceptance are separate evidence. Sensor hardware, real OS revocation,
resume behavior, and the full private-runtime transport remain pending physical
Windows verification. Existing release archives and historical receipts retain
their original version and source identities.
