# Compatibility and test evidence

This table separates historical evidence from new candidates. Reference date:
2026-10-28. Recheck the exact installed controller/server combination when
qualifying a release; protocol support alone does not prove a working UI.

| Combination / function | Evidence | Limits |
|---|---|---|
| Aqara G2H `lumi.camera.gwagl02`, hardware 1.0, Aqara host 2.2.7 | Development camera tested | Other G2H variants and G2H Pro untested |
| JN5189 standalone OpenThread FTD | Router operation observed | A mesh router; not a Thread Border Router |
| HA 2026.10.2 + 0.5.3 firmware, project extension disabled | Native audio/video/Thread mode selections exercised, physical readbacks and full video boots verified | No native camera entity; cached video state required manual re-interview; see [native-only trial](NATIVE-HOME-ASSISTANT.md) |
| HA 2026.10.2 + Matter server schema 13 + project custom camera integration | User confirmed moving live video; core pairing retained | Custom integration required for that tested UI path; native camera UI not established by this result |
| Matter server camera signaling schema 12+ | Signaling path used during development | Schema number alone does not establish snapshot convenience API support |
| Schema 13 raw standard snapshot commands | Development route implemented | Do not require schema 14 solely for the convenience `camera_snapshot` API |
| Native HA camera UI | HA 2026.10.2 Matter discovery has no camera platform; newer versions not qualified | Server camera protocol support alone does not provide an HA camera entity |
| Browser media compatibility | Safari codec investigation and working HA live-view confirmation recorded during development | Exact browser/OS version matrix not retained; new combinations need tests |
| RTSP 1080p Main + 720p Constrained Baseline | Decoded frames verified in development | Vendor camera software must be replaced only during a full boot |
| Snapshot, microphone, talkback, Matter privacy | Historical live development checks reported | New source changes require repeated-session, disconnect, resource and reboot tests |
| Native HomeKit discovery suppression | `_hap` to `_xap` advertisement change verified | Does not terminate the HAP server or prove existing pairings cannot connect |
| Portable source build / media broker candidate | Separate-slot deployment, retained-session reconnects and app restart checks passed; see [candidate report](TEST-REPORT-2026-10-24.md) | Full end-to-end and soak qualification still pending |

## Minimum release qualification

Record firmware/app hashes, HA version, Matter server version and schema,
browser/OS, and the result of each actual test. Check fresh and retained pairing,
reboot, moving video, snapshot decoding, microphone, talkback, privacy scope,
Thread role/freshness, and recovery. Include repeated stream starts/stops,
controller disconnect, LAN outage and bounded memory behavior. A test that was
not performed must stay `not tested`, not `passed`.

Do not put setup codes, Thread datasets, fabric keys, IP-specific logs or captured
private camera images in a public report. Keep device-specific diagnostics private.
