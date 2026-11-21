# Live ALS / brightness monitor

Read-only Mac dashboard at http://127.0.0.1:8765. Requires the connected phone's
`s20` SSH alias. The helper copies the virtual ALS's current event, queries
CoreBrightness and reads `fb.factory` for the driver target. A persistent SSH
stream reuses the helper process and HID/BrightnessSystem clients; it does not
launch SSH or a phone process for every sample.

From the repository root:

```sh
xnu-deps/host/bin/xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 -framework CoreFoundation -framework IOKit -lobjc -o /tmp/als-readings tools/als-monitor/readings.c
scp -O /tmp/als-readings s20:/var/tmp/codex-als-live-readings
python3 tools/als-monitor/server.py
```

Open the dashboard in a browser. Stop the server with Ctrl-C. Readings are saved
to `readings.jsonl` beside the server. The port binds only to localhost. Nits are
software reports/targets, not measured panel luminance. The legacy manual-slider
path can update the panel without updating CoreBrightness's cached nits.
