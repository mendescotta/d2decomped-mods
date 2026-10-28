# tests/

One `test_<subject>.cpp` per subject, each an independent executable and
one ctest entry (see `d2d_add_test` in `CMakeLists.txt`). No fixtures, no
framework — a `main()` that returns 0 on success, checking with plain
`assert` (CMakeLists.txt keeps NDEBUG off for this directory, so asserts
stay on in Release builds); nothing fancier is warranted yet.

Tests that need real assets (D2 ISOs, extracted MPQs, …) locate them at
runtime and print a skip line (`SKIP: ...`) when absent, so `ctest` stays
green on a bare checkout. ctest counts such a test as passed, so a green
run without the data has run none of the data-backed checks: `ctest -V`
(or `build/Testing/Temporary/LastTest.log`) shows the skip lines.

Run: `ctest --test-dir build --output-on-failure`.

Fast act checks (need the MPQs and `D2_PATCH_INSTALLER`): the quests of
Acts 1–5 played headless, an act per test, side by side:
`ctest --test-dir build -R quests_act -j3`. Each act's levels against
game.exe (~15 s for 2 seeds): `ACT=2 tools/emu/sweep.sh 1-2 tiles units monsters`.
