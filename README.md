# ARDUnia Internet Radio

An ESP8266-based Internet Radio receiver using an ST7735S 160×128 TFT display, MAX98357A I2S digital audio amplifier, rotary encoder, and a Wi-Fi setup portal.

This project is part of the **ARDUnia** open-source electronics projects.

## Version

**v2.0.0 — Stable Release**

This release is based on the stable V6.7 code and includes the charging mode and improved buffer/volume display.

## Features

- ESP8266 / NodeMCU based Internet Radio
- MP3 Internet stream decoding with ESP8266Audio
- MAX98357A I2S digital audio output
- ST7735S 160×128 TFT display
- Rotary encoder station navigation
- Rotary encoder mute/menu control
- Analog volume control
- Wi-Fi setup through an ESP8266 Access Point
- Wi-Fi credentials stored in EEPROM
- Automatic Wi-Fi setup after repeated connection failures
- NTP clock synchronization
- Jalali (Persian) date display
- English display interface
- ICY / StreamTitle metadata
- Automatic stream reconnection
- Audio buffer level indication
- Wi-Fi signal indication
- Startup memory menu
- Memory reset option
- 90-minute charging mode
- Charging animation and countdown
- Charging mode can be interrupted with the rotary encoder
- A charging session continues if the device remains powered and the user returns to charging mode
- Charging state is intentionally kept in RAM; after power-off, a new 90-minute charging session starts
- Charge-complete audio notification
- Reduced TFT redraw to minimize visible flicker

## Hardware

| Component | Description |
|---|---|
| ESP8266 NodeMCU | Main controller |
| ST7735S | 1.8-inch 160×128 TFT |
| MAX98357A | I2S Class-D mono amplifier |
| Rotary Encoder | KY-040 or compatible module with pull-up resistors |
| Potentiometer | Volume control through A0 |
| Speaker | Connected to MAX98357A |

See [`docs/HARDWARE.md`](docs/HARDWARE.md) for the complete wiring table.

## Rotary Encoder Pins

| Encoder | ESP8266 |
|---|---|
| CLK | D6 / GPIO12 |
| DT | D0 / GPIO16 |
| SW | D3 / GPIO0 |
| + | 3.3V |
| GND | GND |

The encoder module should have pull-up resistors. GPIO16 does not provide the normal internal pull-up used by other ESP8266 GPIOs.

## Display Pins

| ST7735S | ESP8266 |
|---|---|
| CS | D1 / GPIO5 |
| DC | D2 / GPIO4 |
| RST | Board RST |
| SCK | D5 / GPIO14 |
| MOSI | D7 / GPIO13 |

The display resolution is **160×128** and the project uses landscape orientation.

## MAX98357A Pins

| MAX98357A | ESP8266 |
|---|---|
| BCLK | D8 / GPIO15 |
| LRC / LCLK | D4 / GPIO2 |
| DIN | RX / GPIO3 |
| SD | 3.3V |
| GAIN | Leave floating |
| GND | GND |

Connect the speaker only to the MAX98357A speaker output.

## Software Requirements

### Arduino Libraries

Install:

1. ESP8266Audio
2. Adafruit GFX Library
3. Adafruit ST7735 and ST7789 Library
4. PersianDate by ARDUnia
5. ArduinoJson 6.x

The ESP8266 Arduino core is also required.

### Board

Recommended board:

**Generic ESP8266 Module**

Use settings appropriate for your ESP8266 module and flash size.

## First Wi-Fi Setup

No Wi-Fi SSID or password is hard-coded in the source.

On the first boot, or after repeated failed Wi-Fi connection attempts, the ESP8266 starts:

`InternetRadio-Setup`

Connect to this access point from a phone or computer and open:

`192.168.4.1`

Select the desired Wi-Fi network, enter the password, and save the configuration.

The credentials are stored in EEPROM and are used automatically on subsequent boots.

## Startup Menu

After power-up, the project provides a startup menu allowing the user to:

1. Continue with saved memory
2. Clear saved memory
3. Enter charging mode

The memory reset clears the saved Wi-Fi credentials and saved project data.

## Charging Mode

Charging mode is designed for hardware configurations where the ESP8266 cannot directly measure battery state.

- Internet and Wi-Fi are disabled.
- Radio playback is stopped.
- A 90-minute countdown is used as the charging period.
- The display shows a battery animation, charge percentage and remaining time.
- Pressing the rotary encoder returns to the startup menu.
- Selecting charging again continues the remaining session while the device has remained powered.
- After power-off, the next charging session starts again from 90 minutes.
- At completion, a short audio notification is generated through the existing I2S output.

**Important:** The timer is a software estimate. It does not measure actual battery voltage, current, or state of charge.

## Display

All user-facing display text is in English because the current ST7735S UI does not provide Persian text rendering.

The clock is displayed as:

`HH:MM`

Seconds are intentionally hidden.

## Audio

MP3 decoding is performed by ESP8266Audio. Digital audio is sent through I2S to the MAX98357A.

The project is designed around the MAX98357A digital audio path used in the tested hardware configuration.

## Project Structure

```text
ARDUnia-Internet-Radio/
├── README.md
├── CHANGELOG.md
├── LICENSE
├── docs/
│   └── HARDWARE.md
└── src/
    └── ARDUnia_Internet_Radio_v2.0.0.ino
```

## Status

**Stable / tested hardware release**

The source in this release is the stable version used for the ARDUnia Internet Radio build.

## Author

**ARDUnia / Hamidreza Milaninia**

GitHub: https://github.com/ARDUnia

## License

MIT License. See [`LICENSE`](LICENSE).
