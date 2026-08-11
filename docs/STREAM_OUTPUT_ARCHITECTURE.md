# Stream Output Architecture

## Overview

The plugin owns two peer configuration and health slots: `Primary` and
`Backup`. The first individually started slot uses OBS's built-in streaming
lifecycle. When both are requested, Primary owns the built-in streamer and
Backup starts as an auxiliary native output after OBS reports that Primary's
encoders are ready.

```text
Kaltura API / manual editor
          |
          v
 StreamOutputConfig       StreamOutputConfig
      Primary                  Backup
          |                       |
          v                       v
 OBS built-in streamer      auxiliary native output
 RTMP(S) or SRT             RTMP(S) or SRT
```

All nine Primary/Backup protocol combinations are valid. A protocol change is
applied only to the stopped slot; the other output is not recreated or stopped.
`Start Both` starts Primary through OBS, then starts Backup when the OBS
streaming encoders become available. After startup, either output may be
stopped independently.

## Native OBS strategies

| Protocol | OBS output type | OBS service type |
| --- | --- | --- |
| RTMP | `rtmp_output` | `rtmp_custom` |
| RTMPS | `rtmp_output` | `rtmp_custom` |
| SRT | `ffmpeg_mpegts_muxer` | `rtmp_custom` |

The SRT output is OBS's in-process MPEG-TS muxer. The custom service supplies
the SRT URL, stream ID, and encryption passphrase through libobs connect-info.
There is no child process, custom socket stack, or external FFmpeg binary.

RTMP/RTMPS auxiliary output can share the active OBS encoders. SRT Backup uses
dedicated encoders cloned from the Primary settings so it starts with an
immediate keyframe and independent MPEG-TS headers. The plugin enforces a
two-second live keyframe interval before OBS starts streaming. Shutdown stops
plugin-requested streaming, disconnects signal handlers, and releases the
auxiliary output, service, and dedicated encoder references.

## Configuration and persistence

`StreamOutputConfig` contains the common endpoint, protocol, enabled flag,
reconnect policy, RTMP authentication, and SRT-specific settings. The OBS
project stores independent `primary_output` and `backup_output` objects.

Full destinations, URL query tokens, stream keys, usernames, passwords, SRT
stream IDs, and SRT passphrases are serialized into a per-output secret and
stored by the platform credential backend. OBS project data retains only
query-free destination addresses and credential UUIDs. Legacy SRT stream IDs
are read once for migration but are omitted on the next save.

Kaltura configurations are mapped separately for each role. RTMP is the default
when supplied, followed by RTMPS and then SRT. Selecting a manual configuration
sets `manualOverride`; it remains independent of subsequent edits to the other
slot.

## Health and threading

Each slot reports state, protocol, endpoint, connected state, bitrate, sent
bytes, dropped/total frames, reconnect attempts, connect time, elapsed time,
congestion, and the last libobs error. Metrics unavailable from a given native
output remain unset rather than being synthesized.

OBS signal callbacks only update atomics. The Qt health timer reads the snapshot
on the UI thread, preventing output callbacks from touching widgets directly.
