# Bundled AccuracyCoin

Source: https://github.com/100thCoin/AccuracyCoin

Revision: `46199ae43f52e21df6bb3aef8e395beb7e05b325` (2026-09-18).
Checked against upstream HEAD on 2026-09-20. Upstream does not publish tagged releases.

ROM SHA-256: `fa7f9de92f440d0ba4bfe15551495fdd4432b2404e54a60b1a0a6a0da5fe3a40`.

The ROM, assembly, graphics, README, and LICENSE are unmodified upstream files.
`run_accuracy.c` is MyNES's headless runner. The ROM has 144 pass/fail tests
across 22 menu pages; DRAW entries are informational.

Run `accuracy_coin` from the repository root for all tests, or
`accuracy_coin tests/accuracy_coin/AccuracyCoin.nes 14` for one page.
Any failure, skipped/unrun test, invalid fixture, or timeout returns nonzero.
The runner reads the ROM's menu table and result bytes without changing them.
The runner uses the same default CPU/PPU phase as the frontends.
`NES_CPU_PHASE=0..11` and `NES_ALIGN=0..2` override it through the core's
phase/alignment setters. The default phase passes all 144 tests.
