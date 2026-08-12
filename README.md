# ARDUnia Internet Radio

An ESP8266-based Internet radio project using an ST7735S TFT display, a MAX98357A I2S audio amplifier, and a rotary encoder.

This project is part of the **ARDUnia** open-source electronics framework.

## Features

- Internet radio playback on ESP8266 / NodeMCU
- MP3 stream decoding with ESP8266Audio
- MAX98357A I2S digital audio output
- ST7735S 160×128 TFT display
- Rotary encoder for station navigation
- Mute and volume control
- Wi-Fi signal indicator
- Wi-Fi setup portal using an ESP8266 access point
- Wi-Fi credentials stored in EEPROM
- Automatic Wi-Fi setup after repeated connection failures
- NTP time synchronization
- Persian (Jalali) date display
- English-only display interface
- Automatic stream reconnection
- ICY / StreamTitle metadata support

## Hardware

### Main components

| Component | Description |
|---|---|
| ESP8266 NodeMCU | Main controller |
| ST7735S | 160×128 TFT display |
| MAX98357A | I2S Class-D audio amplifier |
| Rotary Encoder | KY-040 or compatible module |
| Speaker | Connected to MAX98357A |

### Pin assignment

#### ST7735S

| TFT | ESP8266 |
|---|---|
| CS | GPIO5 (D1) |
| DC | GPIO4 (D2) |
| RST | Board RST |
| SCK | GPIO14 (D5) |
| MOSI | GPIO13 (D7) |

#### MAX98357A

| MAX98357A | ESP8266 |
|---|---|
| BCLK | GPIO15 (D8) |
| LRC / LCLK | GPIO2 (D4) |
| DIN | GPIO3 (RX) |

#### Rotary Encoder

| Encoder | ESP8266 |
|---|---|
| CLK | GPIO12 (D6) |
| DT | GPIO16 (D0) |
| SW | GPIO0 (D3) |
| + | 3.3V |
| GND | GND |

The encoder module is assumed to have its own pull-up resistors.

## Required Arduino Libraries

Install the following libraries through the Arduino Library Manager:

- ESP8266Audio
- Adafruit GFX Library
- Adafruit ST7735 and ST7789 Library
- ArduinoJson 6.x
- PersianDate by ARDUnia

The ESP8266 core is also required.

## Arduino IDE

The project was developed and tested with an ESP8266 NodeMCU environment.

Recommended board selection:

**Generic ESP8266 Module**

Use settings appropriate for your ESP8266 board and flash size.

## First Wi-Fi Setup

The project does not require an SSID or password to be hard-coded into the source code.

On the first boot, or after repeated failed connection attempts, the ESP8266 starts a Wi-Fi setup access point:

`InternetRadio-Setup`

Connect to this network using a phone or computer and open:

`192.168.4.1`

Select the desired Wi-Fi network, enter its password, and save the settings.

The credentials are stored in EEPROM and are used automatically on subsequent boots.

## Display

The TFT interface uses English text because the ST7735S display configuration in this project does not provide Persian text rendering.

The clock is displayed as:

`HH:MM`

Seconds are intentionally not displayed.

## Audio

The project uses ESP8266Audio for MP3 decoding and sends digital audio through I2S to the MAX98357A.

The MAX98357A should be powered according to its module specifications. Connect the speaker to the amplifier output and do not connect the speaker output directly to ESP8266 GPIO pins.

## Project Status

This is the initial public version of the ARDUnia Internet Radio project.

The current release focuses on reliable Internet radio playback, Wi-Fi configuration, display, audio output, and rotary encoder control.

## License

This project is released under the MIT License. See [LICENSE](LICENSE).

## Author

**ARDUnia**

Open-source electronics projects and Arduino/ESP development.

