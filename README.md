How to open this project in VS Code Wokwi extension

1. Install the Wokwi VS Code extension.
2. Open this folder in VS Code: `wokwi-esp32-project`.
3. In Command Palette (Ctrl+Shift+P) run: `Wokwi: Open Project` and select `project.json` in this folder, or run `Wokwi: Start Simulation`.
4. If libraries are missing, add them via the simulator's Libraries UI: `PubSubClient`, `ArduinoJson`, `DHT sensor library`, `Preferences`.

PlatformIO (local build)

This repo includes a `platformio.ini` so you can build locally with PlatformIO. Note: the current automated environment does not have PlatformIO or `arduino-cli` installed, so compilation must be run on your machine.

Install PlatformIO CLI or use VS Code PlatformIO extension, then run from this folder:

```powershell
cd "wokwi-esp32-project"
pio run
```

That will fetch the `espressif32` platform and the listed libraries. If you want to just run the serial monitor after flashing/simulation:

```powershell
pio device monitor -b 115200
```

Notes:
- The code targets an ESP32 dev board with DHT22 sensors on GPIO4 and GPIO16, MQ135 on GPIO34, relays on 25/26/27.
- To simulate sensors in Wokwi, add DHT parts in the wiring editor or use the right-side "Parts" panel.
