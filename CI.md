# Tests and native build workflows

## Workflow installation

The downloadable source ZIP already places `.github/` at its repository root. For manual integration, copy the entire `Source/.github/` directory to the repository root's `.github/`, including workflows and `.github/actions/setup/`. GitHub ignores workflow files nested under Source. The scripts automatically find one source root at the repository root, `Source/`, `discord-foobar-rpc/Source/`, or `Discord Music Tracker/Source/`.

Native Windows and macOS project files live under `build/`; automation scripts, dependency pins and triplets live under `tools/`.

Both workflows trigger on push, pull request and manual dispatch. Permissions are `contents: read`. Pull-request code runs on ordinary `pull_request`, not a privileged `pull_request_target` event. Successful native jobs upload their verified addon artifacts with `actions/upload-artifact@v4`. There are no publish, release, push or repository-write steps.

## Build matrix

| Runner | Native architecture | Configurations | Addon output |
|---|---|---|---|
| windows-2022 | x86 / Win32 | Debug, Release | `discord-foobar-rpc.dll` |
| windows-2022 | x64 | Debug, Release | `discord-foobar-rpc.dll` |
| macos-15-intel | x86_64 | Debug, Release | `discord-foobar-rpc.component` |
| macos-15 | arm64 | Debug, Release | `discord-foobar-rpc.component` |

Each job runs 16 Python CI-helper contract tests and seven CTest executables, then builds the native addon. Tests must pass before the addon build runs. Mac sanitizer jobs additionally run AddressSanitizer/UndefinedBehaviorSanitizer or ThreadSanitizer on the production core and tests.

Windows uses the official SDK library project references and Visual Studio 2022/MSBuild. macOS uses the SDK library projects in the workspace and a shared component scheme. Build inspection checks the real exported `foobar2000_get_interface` entrypoint; Windows inspection also checks the DLL architecture/type. Mac inspection checks architecture and verifies an ad-hoc signature after compilation. These are binary structural checks, not proof of loading inside foobar.

## Test coverage

| Suite | Coverage |
|---|---|
| core | Song/artist precedence, missing-field fallback, UTF-8 limits, elapsed/remaining formatting, stable clocks, stream song changes clearing inherited IDs/album context, seek/track/duration reanchoring, pause/resume/stop, invalid lengths |
| discord | Fragmented frames, READY handshake, selected application IDs, null acknowledgments, ping/pong, duplicate suppression, throttling, resend, backoff/replay, application switch, ERROR/oversized frames, handshake timeout, shutdown clear |
| musicbrainz | MBID resolution, complete-tag artwork lookups, disk URL reuse, album URL sharing, compact entity reuse, bounded eviction/expiry and summary-to-track upgrades, escaped queries, ambiguous matches, missing song/artist recovery, actual duration evidence, front-cover/HTTPS selection, release-group fallback, rate limits and Retry-After, malformed/transient responses, cancellation |
| settings | Empty default override, toggle/custom-ID persistence through the shared settings policy, invalid IDs, storage failure notification, portable-profile precedence, AppData/Application Support fallback, unwritable directories |
| cache | Persistent URL/metadata round trips, expiry, negative artwork records, corrupt/mismatched/oversized records, unsafe URLs, disabled/unwritable storage, bounded eviction preserving unrelated files, batched maintenance, exact record expiry |
| service | Slow obsolete lookups, stale-result rejection, disable/enable and cancellation, offline metadata fallback, custom-ID reconnection without redundant lookup, shutdown and idempotent stop, memoized keys, expiry-driven refresh |
| native_ipc | Real Windows named-pipe or Unix-socket exchange: fragmented handshake, Listening payload/progress, process ID, acknowledgments, ping/pong and clear; missing endpoints fail promptly |
| Python contracts | Pinned dependencies, download checksums, native 7-Zip discovery/extraction/provisioning, SDK include setup, Debug/Release curl library layout, read-only workflow matrix and Python steps, native build command/inspection checks, all-target artifact staging/checksums, macOS mode preservation, native project references, PE architecture/export checks |

Network metadata fixtures are injected at the production request boundary; a fake monotonic clock verifies rate limiting without long sleeps. Native IPC uses an isolated test endpoint instead of Discord's live endpoints. Production behavior retains default IPC discovery and real request timing. All tests operate on the production modules; native settings tests exercise the shared policy with a fake store, while the native builds compile the actual SDK adapters and preferences pages.

Both workflows use the same composite dependency setup action. It reads the vcpkg revision from the dependency manifest. Sanitizer jobs use `--tests-only`, avoiding the unused foobar SDK download and native-only setup.

## Dependency provisioning

`tools/dependencies.json` pins the official SDK 2026-10-01 URL/SHA-256 and the vcpkg 2026.07.29 commit. The SDK download fails on any checksum mismatch. SDK extraction uses native 7-Zip because the archive contains BCJ2-compressed data. Windows uses the runner’s installed `7z.exe`; macOS uses `7zz`, installing Homebrew `sevenzip` if needed. Python `py7zr` is not used. vcpkg installs nlohmann/json on both platforms, and WTL plus static curl/zlib with Schannel on Windows. CI supplies x86/x64 triplets with static dependency libraries and the DLL CRT. Downloaded SDK project include paths are adjusted only inside that job's dependency workspace to find WTL. Source/repository project files are not edited by CI.

The runner downloads tools/dependencies during provisioning; the tests themselves do not call MusicBrainz, Cover Art Archive or Discord's network services. There are no API keys, Discord credentials or music uploads.

## Local commands

With nlohmann/json and curl installed, portable tests can be run with:

```sh
cmake -S tests -B out/tests -DCMAKE_BUILD_TYPE=Debug
cmake --build out/tests --config Debug --parallel 2
ctest --test-dir out/tests --build-config Debug --output-on-failure
python tests/ci_contract_tests.py
```

The Python contract tests require PyYAML; CI installs pinned PyYAML and provisions native 7-Zip for SDK extraction. Native build preparation and commands are scripted in `tools/prepare.py` and `tools/run.py`; see the workflow steps for platform flags and vcpkg setup.

CTest emits `out/ci/test-results.xml`; failures print their log into the job output. Native DLLs are under `out/<Win32|x64>/<Debug|Release>/`. Mac bundles are under `out/xcode/Build/Products/<Debug|Release>/`. After binary inspection succeeds, `tools/run.py` stages one artifact under `out/artifacts/discord-foobar-rpc-<platform>-<arch>-<configuration>/`. Windows stages the real DLL; macOS archives the complete `.component` bundle as a tar.gz to preserve executable permissions, bundle contents and signing data during artifact download. Each artifact includes `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `SHA256SUMS.txt`. A missing or empty native binary fails staging.

Download the matching artifact from the workflow run’s **Artifacts** section. All eight Debug/Release matrix jobs upload separate artifacts, retained for 30 days. Use Release for normal testing. Extract the outer GitHub artifact ZIP, then on macOS run `tar -xzf discord-foobar-rpc.component.tar.gz`. Upload only runs after the current job’s tests, build and inspection succeed; a failed platform does not cancel the remaining matrix jobs. No GitHub release is published.

## Validation limits

Portable C++ tests and the Python contracts were executed locally. This execution environment denies socket creation, so the full native IPC exchange was blocked here; it remains required in CI. Neither Windows/MSBuild nor macOS/Xcode is available locally, and no GitHub workflow has been launched. The first CI runs must verify those native builds and sanitizers. Real foobar loading, SDK-store persistence and preferences GUI interaction remain desktop smoke tests.
