# Redtooth

Open-source Bluetooth mod for the original Virtual Boy controller, using an Adafruit ESP32-C6 Feather.

<p><img src="IMG_0148.jpeg" width="32%"> <img src="IMG_0149.jpeg" width="32%"> <img src="IMG_0150.jpeg" width="32%"></p>

## Modes

Hold a button while switching on; holding none keeps the last mode. The LED blinks while waiting for a connection.

| Hold | LED   | Acts as                                         |
|------|-------|-------------------------------------------------|
| L    | blue  | generic BLE gamepad (Android)                   |
| R    | green | Xbox Wireless Controller, e.g. for a Windows PC |
| A    | red   | Switch 2 Pro Controller                         |

Xbox mode also works through an 8BitDo Wireless USB Adapter 2.

## Controls

The right D-pad is the right stick. The left D-pad is the D-pad, or the left stick after <kbd>Select</kbd> + <kbd>L</kbd>.

| Shortcut                                | Does                             |
|-----------------------------------------|----------------------------------|
| <kbd>Select</kbd> + <kbd>L</kbd>        | left D-pad: D-pad ↔ left stick   |
| <kbd>Select</kbd> + <kbd>B</kbd>        | swap A and B                     |
| <kbd>Select</kbd> + <kbd>A</kbd> for 3 s | forget all pairings             |

Extra buttons in Xbox and Switch 2 mode:

| Virtual Boy                          | Xbox  | Switch 2 |
|--------------------------------------|-------|----------|
| <kbd>Start</kbd>                     | Menu  | +        |
| <kbd>Select</kbd>                    | View  | −        |
| <kbd>Select</kbd> + <kbd>Start</kbd> | Xbox  | HOME     |
| <kbd>Select</kbd> + <kbd>R</kbd>     | Share | Capture  |

## Wiring

![Wiring](docs/wiring.svg)

Wire colours of the original controller cable; the controller runs fine on the Feather's 3 V.

The red wire originally carried the battery pack's power through the power switch to the console. Cut it inside the
controller and wire the end that comes from the switch to IO7. Switching off then connects IO7 to GND and the Feather,
which keeps running on its own LiPo, goes into deep sleep. It has to be IO7: only IO0–IO7 can wake the C6.

Drawn by `docs/wiring.py`.

## Mounting and battery pack cover

The Feather and the LiPo sit in the controller's battery pack compartment, held with double-sided tape, the Feather
centred with its USB-C port at the top. The wires reach the inside of the controller through its battery contact holes,
so the controller's shell needs no cutting or drilling.

`batteryPackCover/` holds the 3D-printable cover that slides over the compartment in place of the original battery pack,
with an opening for the USB-C port: the FreeCAD model (`MainBody.FCStd`) and a 3MF ready to print (`MainBody-Body.3mf`).

## Build

Firmware in `idf/`, for ESP-IDF 6.1: `cd idf`, then `idf.py build flash`.

The Switch 2 needs a 5 ms connection interval, below the Bluetooth LE minimum. `idf/cmake/ble_interval_patch.cmake`
allows it by patching one instruction in a build-local copy of `libble_app.a`; ESP-IDF itself is not changed.

## Credits

Switch 2 protocol ported from [ESP32-BLE5-NSController-Emulator](https://github.com/2017fighting/ESP32-BLE5-NSController-Emulator)
(MIT, `idf/main/switch2/LICENSE`), based on [switch2_controller_research](https://github.com/ndeadly/switch2_controller_research).
Feather drawing in `docs/wiring.svg` from [Adafruit's Fritzing library](https://github.com/adafruit/Fritzing-Library) (CC BY-SA 3.0).
