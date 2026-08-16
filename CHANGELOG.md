# Changelog

## [2.0.0] - 2026-08-16

### Added
- Startup memory menu.
- Saved-memory reset option.
- 90-minute charging mode.
- Charging countdown and battery animation.
- Charge-complete audio notification.
- Return from charging mode to the startup menu using the rotary encoder.
- Continued charging session while the device remains powered.
- English-only charging interface.
- Improved buffer and volume bars using more of the 160-pixel display width.

### Improved
- Reduced TFT redraw in charging mode to eliminate visible flicker.
- Centered charging status elements on the 160×128 display.
- Preserved the stable V6.7 audio path and 1 KB preallocated audio buffer.

### Compatibility
- ESP8266 / NodeMCU
- ST7735S 160×128 TFT
- MAX98357A I2S amplifier
- Rotary encoder with pull-up resistors

### Notes
- Charging mode is a software timer and does not measure actual battery state.
- A power cycle starts a new 90-minute charging session.
- The radio station list is intentionally not documented as an update feature in this release.
