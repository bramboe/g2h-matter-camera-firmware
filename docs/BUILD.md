# Build from source and prepare a release

The application is an ARMv7 Linux/musl static executable. It is separate from the
JN5189 radio firmware and from Aqara's camera application. This recipe does not
connect to a camera, install software, publish artifacts, or flash a radio.

## Inputs and provenance

`build/inputs.lock.json` pins the Matter SDK commit, libdatachannel commit,
FFmpeg/Opus/OpenSSL archives and recorded tool versions. Matter submodules use the
revisions in that commit's gitlinks. Initialize them with the SDK's own checkout
instructions; do not silently replace them with the newest versions. ZAP is
`v2025.10.23-nightly`; GN was `2179 (05eed8f6252e)`; Zig must be `0.14.1`.
Use Python 3.12+, CMake 3.21+, Ninja, Make, Perl, and the SDK's Python generator
dependencies. The original build host was macOS ARM64; a fresh Linux build is not
yet qualified. Tool downloads and SDK Python environment setup are not automated
or silently installed by this repository.

```sh
git clone https://github.com/project-chip/connectedhomeip.git matter-sdk
git -C matter-sdk checkout abcc720b48c5e59c0edcfe65c516f76ca9448aa3
# Initialize required submodules using the pinned SDK's checkout instructions.
# Activate the SDK's Python tooling and put GN, ZAP, CMake and Ninja on PATH.
export ZIG=/path/to/zig-0.14.1/zig
export ZAP_INSTALL_PATH=/path/to/zap-v2025.10.23-nightly
python3 tools/build.py --matter-root ./matter-sdk --stage check
```

The `check` stage verifies revisions and Zig only. It is not a successful build.
The SDK tree must be dedicated to this project: `prepare` applies the retained
one-line SDK patch which value-initializes snapshot allocation arguments. It
accepts an already-applied exact patch; it never resets or cleans a checkout.

## Build

```sh
python3 tools/build.py --matter-root ./matter-sdk --stage all --fetch --jobs 4
```

`--fetch` explicitly permits downloads of the hash-pinned FFmpeg, Opus and
OpenSSL archives. Without it, place those archives in
`builds/source/downloads/` first. They are verified before extraction.
Individual stages are `prepare`, `media`, `crypto`, `rtc`, `sdk`, and `app`, in
that order. Existing retained local libraries are not silently imported. The
output is `builds/source/app/g2h-matter-camera`; if its source is present, the
separate `g2h-media-broker` is also built. Failed builds stop immediately.

SDK GN configuration and endpoint model live in `build/sdk/`. Cross-compiler
wrappers and CMake settings are in `build/toolchain/`. Media libraries exclude
FFmpeg network protocols, programs, filters, devices and container demuxers;
H.264/AAC decoding, AAC/JPEG encoding, scaling and resampling are enabled.
FFmpeg GPL/nonfree options are not enabled. The application links Opus and
libdatachannel with static OpenSSL for its secure media transport.

## Offline checks and release gate

```sh
python3 tools/check_source.py
python3 -m unittest discover -s tests
python3 tools/release.py stage --build-out builds/source \
  --matter-root ./matter-sdk --dest builds/release-candidate
python3 tools/release.py verify builds/release-candidate/manifest.json
```

The release stage requires an application build record, unchanged source hashes,
and a matching executable hash. It collects the executable, relinkable application
objects and static libraries, corresponding tracked SDK/submodule source, pinned
archive sources, project sources and licenses. It fails when those inputs are
missing. It does not read untracked device identities or camera dumps. The extracted relinking bundle keeps application objects separate from static
libraries. Replace the relevant `libraries/media-prefix/lib/*.a` archives, then
run `ZIG=/path/to/zig python3 recipe/relink.py . --output camera-relinked` from
its root. Rebuilding modified codec sources uses the same `media` stage; retain
its headers/configuration and replacement archive ABI. Keep the
complete release candidate together: distributing only the statically linked
executable does not provide the corresponding source and relinking material
required for the LGPL components. Preserve notices for every dependency; the
project's MIT license does not replace them.

`hardware_qualified` remains false. A valid manifest proves integrity and local
source/build correspondence, not device compatibility, a clean-room rebuild,
security certification, or successful live video. Record actual device test
reports separately before promoting a release. Do not overwrite `app/` or
regenerate repository `SHA256SUMS` merely because an offline build succeeded.

## Current reproducibility limits

The historical executable in `app/` predates this portable recipe. Its checksum
identifies the supplied artifact; it is not evidence of bit-for-bit reproduction
from the present source. The new recipe preserves configurations and inputs but
still needs a clean full build on a second host and runtime qualification. ZAP,
GN and the SDK Python environment are recorded rather than bootstrapped. Build
paths, archive metadata and compiler behavior may affect binary bytes; a matching
functional build is not yet a bit-identical reproducible release.
