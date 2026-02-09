# ESP32 Reader / Mouse / Gamepad

ESP32 firmware that turns a single device into:

- e-ink TXT reader
- Bluetooth mouse
- Bluetooth gamepad

Personal project built for custom hardware.

## Features

- Reads `.txt` files from SD
- Paginated text rendering for e-ink
- Page index cache (`.pgx`)
- Per-book bookmarks (`.bmk`)
- Idle screen and deep sleep
- Physical buttons + analog joystick
- Bluetooth HID (mouse and gamepad)

## Modes

- **Reader**  
  Text reader with pagination and bookmarks.

- **Mouse**  
  Joystick-driven Bluetooth mouse with scroll and clicks.

- **Gamepad**  
  Bluetooth gamepad (hat + buttons).

## Hardware

- ESP32
- Adafruit ThinkInk display
- Adafruit Joy FeatherWing
- SD card

## Notes

- Page cache depends on layout and is rebuilt when needed.
- Default paths:
  - `/books`
  - `/pictures`

## License

[MIT](https://choosealicense.com/licenses/mit/)
