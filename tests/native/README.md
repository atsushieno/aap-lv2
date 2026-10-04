# LV2 state regressions

Run `tests/native/run-state-regression.sh` from the aap-lv2 root. Requires CMake,
a C++20 compiler, and initialized dependency submodules. It builds in a temporary
directory and runs debug/release with UBSan. No Android runtime assets or Gradle
changes are required. `SANITIZERS=address,undefined` enables ASan where its runtime
works; ASan hangs before main on the current macOS host, including a minimal probe.

The test compiles the production extension file and bridge, loads a real LV2
fixture through the vendored Lilv, and exercises:

- State before preparation: omit unavailable controls while retaining plugin state.
- Missing/non-control symbols, absent metadata/value pointers, wrong types and sizes.
- Real Lilv integer, long and double preset literals converted to scalar float controls.
- Processing/deactivation followed by repeated AAP size/save/restore calls.
- A display name containing spaces, control values and plugin-owned state round trips.
- The production factory's options lifetime and state capture after factory return.

This validates the confirmed source faults. It does not reproduce the historical
MDA APK crash or verify worker-using plugins on Android. State/preset operations
remain control operations and require the host to coordinate with processing.
The production state functions stay in `aap-lv2-extensions.cpp`.
