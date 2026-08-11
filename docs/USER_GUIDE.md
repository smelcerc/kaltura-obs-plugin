# User Guide

## Requirements

- OBS Studio 32.0 or newer
- macOS 12+ or a supported 64-bit Linux distribution
- A valid Kaltura Session with access to the relevant live entries
- Enough CPU for local Whisper transcription when captions are enabled

## Install and open

Install the release package, restart OBS, and open **Tools → Kaltura Live Settings…**. The dock can
be shown through **Tools → Open Kaltura Live** or OBS's Docks menu.

On macOS, the installer places the plugin system-wide under `/Library/Application Support/obs-studio/plugins`.
On Debian/Ubuntu, the package installs the module under `/usr/lib/obs-plugins` and its local Whisper
models under `/usr/share/obs/obs-plugins/kaltura-live/models`.

## Connect to Kaltura

1. Paste the KS into the masked **Kaltura Session** field.
2. Select **Validate**, or reopen the page to validate an existing saved KS automatically.
3. Confirm the connected user, partner, and expiration.
4. Search, sort, or filter the live-entry list and select one entry.
5. Select Primary, Backup, or Both and save.

The plugin never displays the KS or stream key in plain text. The selected entry and output routing
are saved with the current OBS project.

## Streaming

- **Primary only:** the normal OBS streaming output targets Kaltura Primary.
- **Backup only:** the normal OBS streaming output targets Kaltura Backup.
- **Both:** Primary uses the normal OBS output and Backup uses a managed auxiliary output. Their
  dock buttons start and stop independently.

Review the confirmation dialog before allowing the plugin to change OBS output settings. Use the
provided revert action to restore the previous OBS service configuration.

Green health is connected, yellow is starting/reconnecting/degraded, and red is stopped or failed.

## Local captions

1. Enable captions before starting an output.
2. Choose Tiny for lower CPU usage or Base for improved accuracy.
3. Set a whole-second program delay. This delays outgoing audio/video while captions are generated.
4. Choose the CEA-608 layout, placement, and alignment.
5. Check the transcript preview in Settings and the CEA-608 output monitor in the dock.

Long transcription results are split across consecutive CEA-608 screens; text beyond two display
rows is not discarded. Caption settings lock while an output is active.

The CEA-608 monitor confirms that OBS accepted a screen for native video-frame insertion. It cannot
confirm receipt by the remote player. Use **Copy All** to copy retained output history. Use
**Copy Transcript** in Settings to compare it with the Whisper results.

## Custom dictionary

The CSV requires `preferred_text`; `spoken_form` is optional:

```csv
spoken_form,preferred_text
cal torah,Kaltura
,RTMP
```

Preferred text supplies Whisper vocabulary context. When spoken form is present, matching final
transcript text is replaced deterministically. A sample is available from Settings or at
[`sample-caption-dictionary.csv`](sample-caption-dictionary.csv).

## Troubleshooting

- **Plugin is absent:** verify OBS 31+, reinstall the correct architecture, and inspect the OBS log.
- **KS validation fails:** confirm network access and that the KS is current and authorized.
- **Captions are late:** increase program delay before starting the output or select Tiny.
- **Transcript exists but monitor does not:** copy both histories and inspect caption health/drops.
- **Monitor matches but player differs:** inspect RTMP reconnects and the downstream caption/VTT path.

Before sharing diagnostics publicly, remove KS values, stream URLs containing query tokens, user
names, partner information, and local filesystem paths.
