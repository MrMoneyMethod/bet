# Third-party notices

## Vendored application dependencies

### cpp-httplib

- Repository: https://github.com/yhirose/cpp-httplib
- Version: **v0.22.0**; commit `b6c55c6030f5160d1a360a5a5180ba3205e2ce2f`.
- License: MIT, copyright Yuji Hirose.
- Files: `vendor/httplib/httplib.h`, `vendor/httplib/LICENSE`.
- Header SHA-256: `f81548031db8543a46170431afac1d2d9b305fc52cfd356a672de6688c2bf697`.
- Used for the local HTTP server. No OpenSSL/TLS cpp-httplib client is enabled.

### JSON for Modern C++ (nlohmann/json)

- Repository: https://github.com/nlohmann/json
- Version: **v3.12.0**; commit `55f93686c01528224f448c19128836e7df245f72`.
- License: MIT, copyright Niels Lohmann.
- Files: `vendor/nlohmann/json.hpp`, `vendor/nlohmann/LICENSE.MIT`.
- Header SHA-256: `c3f4ea9f906bb0179384f2075dd3bc22abbb6d9e67c56c1c00c85a89138c656f`.
- Used for feed parsing, validation, state and persistence.

License notices in the vendored files are retained. No source changes were made to these two headers.

## Cross-built Windows binary runtime notices

The distributed cross-build uses Zig **0.13.0**, bundled MinGW-w64 components and LLVM C++/unwind runtime components. Copies of the applicable bundled license texts are in `vendor/runtime-licenses/` and in the Windows ZIP:

- `Zig-MIT.txt` — Zig contributors, MIT (including compiler-runtime portions as applicable).
- `MinGW-w64-COPYING.txt` — MinGW-w64's component-specific copyright/license notices.
- `LLVM-libcxx.txt`, `LLVM-libcxxabi.txt`, `LLVM-libunwind.txt` — Apache 2.0 with LLVM exceptions, plus included component notices where applicable.

The compiler itself is not redistributed. A native Visual Studio build uses the toolchain's own runtime/license terms instead. System WinHTTP, Shell32, Winsock and Universal CRT DLLs remain part of Windows; they are not bundled or replaced by this project.

## Research references are not dependencies

`nba_api`, basketball-reference scrapers and betting-model repositories were studied for API schemas, data separation and conceptual baselines. They are not imported as Python packages, and their trained models, proprietary odds data or unlicensed code are not included. New adapter and model implementations are original C++ code. The reference scan and license decisions are in `docs/GITHUB_REFERENCES.md`.

Public provider names/teams/player names describe the subject of research and do not imply affiliation, sponsorship or data-redistribution permission. The UI uses initials and locally drawn icons, not copied provider logos/artwork. No web fonts or external images are shipped.
