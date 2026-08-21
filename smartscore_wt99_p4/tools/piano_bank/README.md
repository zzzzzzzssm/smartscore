# SmartScore WT99 piano bank

`build_salamander_bank.py` creates the SD-card piano resource used by the
local-score preview player.

## Build

Install either FFmpeg, or the Python packages `numpy`, `scipy`, and
`soundfile`, then run:

```powershell
python tools/piano_bank/build_salamander_bank.py --download
```

The script downloads only the selected Salamander Grand Piano V3 notes and
velocity layers, converts them to 24 kHz mono PCM, precomputes seamless loop
crossfades, applies the device's approximately +2 dB sample-bank gain, writes
CRC-protected zone metadata, and verifies the result.

Copy the generated file:

```text
sdcard/smartscore/piano.pbank
```

to this exact path on the device's MicroSD card:

```text
/smartscore/piano.pbank
```

After inserting the card, restart the device so the existing global SD mount
runs before the display and score player start.

The source FLAC cache is intentionally ignored and is not needed on the
device. To validate an existing bank without downloading or rebuilding it:

```powershell
python tools/piano_bank/build_salamander_bank.py --verify-only
```
