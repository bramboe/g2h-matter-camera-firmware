# Third-party notices and source material

The project's MIT license applies to original project code, not to linked
libraries or copied SDK build files. `build/sdk/build_overrides/` contains Matter
SDK build configuration with its original Apache 2.0 notices. The SDK snapshot
initialization patch modifies Apache 2.0 source. Preserve those notices.

The application statically links components with separate licenses, including:

- Matter SDK: Apache 2.0 (`Matter-Apache-2.0`).
- FFmpeg: LGPL 2.1 or later under the restricted build options in `tools/build.py`.
- Opus: BSD-style license (`Opus-COPYING`).
- libdatachannel: MPL 2.0 (`libdatachannel-MPL-2.0`).
- OpenSSL: Apache 2.0 (`OpenSSL-LICENSE.txt`).
- libdatachannel's pinned libjuice, libsrtp, usrsctp and other dependencies, and
  Matter SDK dependencies: retain the license/notice files in their corresponding
  source trees. Their inclusion is not relicensed by this project.

`tools/release.py stage` requires corresponding-source archives and relinkable
objects/static libraries alongside the executable. Do not remove source files,
notices, modified-library sources, or build configuration from that bundle. The
archive pins identify source inputs; they do not imply a cryptographically
verified upstream signature when the lock explicitly says otherwise.

## Static C/C++ runtime provided by Zig 0.14.1

The ARM Linux/musl source build uses Zig's bundled C/C++ runtime rather than
linking the G2H vendor's system C library. Preserve these runtime notices with
the executable and source/relinking bundle:

- `Zig-LICENSE`: Zig contributors' MIT license, including Zig runtime code.
- `musl-COPYRIGHT`: the bundled musl library's full copyright/license notice and
  its contributor/third-party attribution. `musl-additional-notices.txt` retains
  unmodified copyright and license comment blocks from the bundled musl sources,
  including permissively licensed math implementations. It is a broad source
  notice collection, not a claim that all listed implementations are linked.
- `libcxx-LICENSE.TXT`: the full libc++ license, including LLVM exceptions and
  retained legacy license sections.
- `libcxxabi-LICENSE.TXT`: the full libc++abi license.
- `libunwind-LICENSE.TXT`: the full LLVM unwinding-library license.

These texts were copied from the retained official Zig 0.14.1 distribution.
`runtime-provenance.json` records their upstream relative paths and SHA-256
hashes. Compiler-built runtime support and libc++ remain under their upstream
licenses; the application MIT license does not replace those terms. The compiler
executable itself is not redistributed in the application release.
