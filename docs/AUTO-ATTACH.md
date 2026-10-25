# Automatic Home Assistant attachment

Install the project extension once, enable the observer below, then pair the G2H
through Home Assistant's normal **Add Matter device** flow. Within 30 seconds
of the paired camera appearing in the Matter client cache, the extension
verifies its live identity and creates the camera/motion/Thread platforms.
There is no second pairing, setup code, RTSP URL or network key to enter.

This is project extension code, not native camera support added to HA Core.
In HA 2026.10.2, device records belong to their config entries: Matter and this
extension appear as **linked devices** sharing the Matter identifier, rather
than one combined device page. Automatic attachment removes the manual
extension-addition step; it does not merge those pages or modify HA Core.

## Enable once

1. Copy `home-assistant/custom_components/g2h_matter_camera` from this repository
   into the HA configuration's `custom_components` directory.
2. Add this top-level key once to `configuration.yaml`:

   ```yaml
   g2h_matter_camera:
   ```

3. Check the HA configuration and restart HA. An already configured G2H is
   retained, including its entity identities and enabled/disabled settings.
4. Pair a new G2H through the standard Matter flow. Do not add the companion
   extension manually. If the camera is already paired, do not pair it again.

An existing project config entry also loads the observer. The YAML key enables
it on a fresh installation, before any camera/extension entry exists.
Installing files alone does not cause HA to load an arbitrary custom component.

## Matching and recovery

- Only the exact current G2H camera product name is a candidate. Other Matter
  devices and the older standalone Thread-radio pairing are ignored.
- A candidate is verified using live Basic Information and camera server-list
  attributes. Cached names alone cannot create an extension entry.
- Camera signalling requires server schema 12 or newer. Failure creates no
  partial entry and is retried on a subsequent scan.
- Both the Matter entry/node pair and the verified unique ID prevent duplicates.
  Existing disabled extension entries are respected, not re-enabled.
- Configured cameras cause no additional live reads on these scans. The
  30-second scan examines the existing local Matter node cache only.
- A pending scan blocks overlapping scans. Shutdown cancels the observer's own
  timer/task; it never closes the shared Matter client or alters its fabric.
- Removing an extension entry while leaving the observer enabled can attach it
  again. Disable its entry if it should stay disabled, or remove the YAML key
  and all project entries to stop loading the component.

Downloaded project diagnostics include bounded observer counters. They contain
no raw descriptors, fabric credentials, server URLs or Thread datasets.

## Validation limits

Synthetic tests cover startup before Matter is ready, later pairing, live
identity mismatch, unsupported server API, disabled/existing entries, unique-ID
deduplication, failed reads, overlapping scans and shutdown cleanup. A runtime
upgrade of an already configured camera verifies reuse, not a fresh real-device
commissioning test. It never removes an existing pairing to manufacture one.

On the installed 0.4.1 extension, HA configuration validation and restart passed.
The observer completed three initial cache scans with zero failures and zero
new entries for the already configured camera. The existing device/entry/entity
identities survived; diagnostics reported the camera online, fresh motion, and
a Thread router on channel 25. This is a runtime reuse check; initial creation
of a newly commissioned physical camera is covered only by synthetic tests.
