# SRT Support

SRT is a first-class output protocol for both Primary and Backup. It may be used
on either slot while the other slot uses RTMP, RTMPS, or SRT.

## Settings

When SRT is selected, the dock exposes the latency control directly. It accepts
250–8000 ms and ignores mouse-wheel page scrolling so the setting cannot change
accidentally. Endpoint, Stream ID, mode, encryption, packet-size, timeout, and
reconnect values are populated from Kaltura and retained internally; the
connection-details and advanced editors remain hidden in this release.

The active Kaltura URI builder emits
`[SRT URL]?streamid=[SRT Stream ID]&latency=[microseconds]`. The Stream ID is appended verbatim so its
`#`, `:`, `=`, and comma characters are not percent-encoded. OBS supplies caller
mode and other transport defaults without additional URL parameters. Latency is
configured in the dock from 250–8000 ms (default 3000 ms) and converted to the
microseconds expected by FFmpeg's SRT protocol. The OBS Stream
Key field is deliberately empty because the Stream ID is already present in the
URL.

When SRT Backup runs alongside Primary, Backup uses a dedicated encoder instance.
This guarantees an immediate keyframe and repeated MPEG-TS headers instead of
joining Primary's active encoder in the middle of its GOP.

## Kaltura mapping

`primarySrtBroadcastingUrl` and `secondarySrtBroadcastingUrl` map independently
to Primary and Backup. `primarySrtStreamId` and `secondarySrtStreamId` remain
distinct. If SRT is unavailable for a role, mapping falls back to that role's
RTMPS URL, then RTMP URL.

## Operational notes

- Configure OBS streaming encoders before starting a plugin output.
- A listener URL must identify a local interface/port accepted by OBS's FFmpeg
  build and the operating system firewall.
- Reconnect telemetry comes from libobs's output signal. Lower-level RTT or
  packet-loss metrics are shown only if libobs exposes them; the plugin does not
  invent replacements.
- Credentials and encryption material are stored in the operating system's
  secure credential backend, not OBS project JSON. Destination URL query
  parameters are secured there as well.

The protocol matrix and SRT URI validation are covered by
`kaltura-stream-output-tests`.
