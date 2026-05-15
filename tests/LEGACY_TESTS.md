# Legacy Test Status

These files are intentionally excluded from the active Catch2 suite in `tests/CMakeLists.txt`:

- `tests/src/test_rt_audio_thread_violations.cpp`
- `tests/src/test_unit_parameter_manager.cpp`

They target an older "Morphy" test harness and outdated APIs that no longer match the current `MorePhiProcessor`, threading model, and build graph. Keeping them disabled avoids false failures and duplicate frameworks in CI.

When these tests are revived, they should be migrated to Catch2 and updated against current public interfaces before being re-added to `MorePhiTests`.
