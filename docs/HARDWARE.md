# Hardware Guide

## Main Connections

### ST7735S 160×128 TFT

| TFT Pin | ESP8266 |
|---|---|
| CS | D1 / GPIO5 |
| DC | D2 / GPIO4 |
| RST | Board RST |
| SCK | D5 / GPIO14 |
| SDA / MOSI | D7 / GPIO13 |
| VCC | 3.3V |
| GND | GND |

### MAX98357A

| MAX98357A Pin | ESP8266 |
|---|---|
| BCLK | D8 / GPIO15 |
| LRC / LCLK | D4 / GPIO2 |
| DIN | RX / GPIO3 |
| SD | 3.3V |
| GAIN | Floating |
| GND | GND |

Connect the speaker to the MAX98357A output. Do not connect the speaker output to an ESP8266 GPIO.

### Rotary Encoder

| Encoder Pin | ESP8266 |
|---|---|
| CLK | D6 / GPIO12 |
| DT | D0 / GPIO16 |
| SW | D3 / GPIO0 |
| + | 3.3V |
| GND | GND |

The encoder module is expected to provide pull-up resistors.

### Volume

| Component | ESP8266 |
|---|---|
| Potentiometer wiper | A0 |
| Potentiometer ends | 3.3V / GND |

## Important ESP8266 Pin Notes

- GPIO15 is used by I2S BCLK.
- GPIO2 is used by I2S LRC.
- GPIO3 (RX) is used by I2S DIN.
- GPIO0 is used by the rotary encoder switch and is also a boot-strap pin. Do not hold the encoder switch pressed during reset/programming.
- GPIO16 is used for encoder DT and requires the pull-up provided by the encoder module.
- The ST7735S display pins are fixed in this project.

## Power

Use a suitable regulated supply and follow the voltage requirements of each module.

For battery-powered builds, a 5V boost converter may be used for modules that require 5V. The charging timer in this project is only a user-interface timer; it is not a battery management system.
