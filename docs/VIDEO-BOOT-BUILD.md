# Reproduce the optional video boot gate

The installed video-off control uses our 4,348-byte preload, not the runtime
sensor-stop experiments. The preload is bound to one exact native camera image
and ARM glibc runtime. Its wrapper validates the native/helper profile before
loading it. See [CONTROLS.md](CONTROLS.md) for reboot behavior and observations.

No Aqara image or runtime library is supplied by this repository. Use your own
private, verified copies from the qualified camera. The builder rejects any
native image/runtime hash outside the qualified profile before compilation.
It never connects to the camera, extracts vendor files or installs anything.

With the pinned Zig version in `build/inputs.lock.json`:

```sh
python3 tools/build_video_boot_gate.py \
  --zig /path/to/zig \
  --native-image /path/to/your/camera.secondary-baseline.homekit-off-xap.bin \
  --native-libraries /path/to/your/native-runtime \
  --out builds/video-boot
```

The private runtime directory must contain the exact four library files named
in the builder. Their hashes are checked, as are ELF32 ARM architecture and the
native image SHA-256. These files remain inputs only; neither they nor the
native image are copied to the output directory. The old-loader linker layout
keeps ordinary/PLT relocations contiguous for this glibc 2.19 loader. Exports are
hidden; the constructor installs the exact task-start dispatch in memory.

The output contains our `video-boot-gate.so`, an input/artifact hash manifest,
and compiler caches. A fresh output directory is required so an existing gate
cannot be overwritten accidentally. Compiling with the public recipe on
2026-10-28 reproduced the installed helper byte for byte:

```text
63415147af460eeaa12321bbf0b5fb77757d4e0df3f9d4228f70753ce1783941
```

A successful build is not an installation qualification. The installed backend
also needs a private verified profile, the native startup wrapper and an
independently recoverable full-boot trial. Those were installed and tested on
the qualified camera as documented in [the device report](TEST-REPORT-2026-10-28.md).
Do not copy an experimental helper or bypass profile checks on another image.

For rollback on the qualified camera, retain the private original startup XML,
app backup, helper manifest and explicit recovery instructions. Revert startup
through a full boot; never stop the native camera, monitor or broker in place.
