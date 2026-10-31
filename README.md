# Redtooth

Open-source Bluetooth mod for the original Virtual Boy controller, using an Adafruit ESP32-C6 Feather.

<p><img src="IMG_0148.jpeg" width="32%"> <img src="IMG_0149.jpeg" width="32%"> <img src="IMG_0150.jpeg" width="32%"></p>

## Modes

Hold a button while switching on; holding none keeps the last mode. The LED blinks while waiting for a connection.

| Hold | LED   | Acts as                                              | Pairing                                         |
|------|-------|------------------------------------------------------|-------------------------------------------------|
| L    | blue  | generic BLE gamepad (Android)                        | "Virtual Boy Controller" in Bluetooth settings  |
| R    | green | Xbox controller, for an 8BitDo Wireless USB Adapter 2 | hold the adapter's button until it blinks fast |
| A    | red   | Switch 2 Pro Controller                              | Change Grip/Order, once; it reconnects by itself after that |

## Controls

Right D-pad = right stick. Select + L: left D-pad as D-pad or left stick. Select + B: swap A and B.
Select + A for 3 s: forget all pairings. Xbox and Switch 2: Start = +, Select = −, Select + Start = HOME, Select + R = Capture.

## Wiring

![Wiring](docs/wiring.svg)

Wire colours of the original controller cable. The controller runs fine on the Feather's 3 V. The red battery line
goes through the controller's power switch; when it reads LOW the Feather goes into deep sleep. Drawn by `docs/wiring.py` (schemdraw).

## Build

Firmware in `idf/`, for ESP-IDF 6.1: `cd idf`, then `idf.py build flash`.

The Switch 2 needs a 5 ms connection interval, below the Bluetooth LE minimum. `idf/cmake/ble_interval_patch.cmake`
allows it by patching one instruction in a build-local copy of `libble_app.a`; ESP-IDF itself is not changed.

`batteryPack/custom/`: FreeCAD model and 3MF print file of the case that replaces the original battery pack and holds the Feather.

## Credits

Switch 2 protocol ported from [ESP32-BLE5-NSController-Emulator](https://github.com/2017fighting/ESP32-BLE5-NSController-Emulator)
(MIT, `idf/main/switch2/LICENSE`), based on [switch2_controller_research](https://github.com/ndeadly/switch2_controller_research).
