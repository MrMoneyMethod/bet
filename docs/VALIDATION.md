# Edgecourt 0.2.0 — build and validation report

**Date:** 2026-10-13 (UTC). Source and release artifact were built in a Linux sandbox. This report separates software tests from real-world prediction validation.

**Incremental verification for the closing-market comparison change:** direct GCC builds of the current core test executable and full native app passed; the core/feed suite reported **66 passed, 0 failed**; the market-comparison CLI sample ran and returned `synthetic: true`; Python history tests reported **19 passed**; the native history and HTTP API smoke scripts passed individually; `node --check web/app.js`, JSON parsing and `git diff --check` passed. CMake is not installed in this turn's sandbox, so the CTest aggregator, browser automation and Windows packaging workflows below were not rerun. No authentic historical market benchmark was available.

## Executed checks

| Check | Result | What it establishes |
|---|---|---|
| C++17 native build, direct GCC (CMake unavailable in this iteration) | Passed | Current core, public-data parsers, server and embedded UI compile together. |
| Core/feed regression executable | **66 passed, 0 failed** (direct GCC run; includes paired closing-market scores, 30-game interval threshold and leakage guards) | Mathematical contracts, parsing, import validation and eligibility guards; not predictive accuracy. |
| Historical tooling unit tests | **19 passed**, synthetic fixtures only (Basketball Reference collector + hoopR CSV ingest) | Schedule/table parsing, comment tables, PBP sequence/overtime/first-FG settlement, score/period completeness, SQLite deduplication, feature cutoff, robots/wildcards, rate spacing, caching, permission and single-writer gates. |
| Native historical pipeline | Passed, no provider requests | Generated saved HTML → SQLite/events → streaming feature export → native regular props and first-FG probabilities only after current lineup import; atomic reload/removal and Demo isolation. |
| CTest constituent suites (invoked individually) | **4/4 passed** | Core/feed, collector, native historical pipeline and real-process HTTP API integration; the CTest runner itself was not invoked because CMake is unavailable. |
| Browser interactions, headless Chromium | Passed, including percentage quotes/input/export and history coverage/reload | Desktop and 390px mobile layouts; no JavaScript errors during the tested interactions. |
| JavaScript syntax, `node --check` | Passed | Embedded frontend syntax. |
| Complete synthetic feature import | Passed | Custom game, two team environments, 40 player logs and paired points quotes yield modeled rows in Demo. |
| Demo → Live → Demo isolation | Passed | Live drops Demo markets, rejects declared-synthetic imports, and switching modes prevents an obsolete live refresh from publishing over Demo. Public-provider responses were not validated. |
| Saved ESPN HTML price import without original timestamp | Rejected as expected | Import receipt time cannot turn an old saved price into a fresh quote. |
| Windows x64 cross-build, Zig 0.13.0 | Passed | Produces a real Windows AMD64 PE executable, not a renamed Linux binary. |
| Windows executable header/import inspection | Passed | x64 PE; Windows/UCRT system DLL imports, no separately installed C++/OpenSSL runtime dependency. |
| Release ZIP integrity and SHA-256 manifests | Checked during packaging | ZIP CRCs, embedded executable identity and external executable/archive digests agree. Hashes do not prove safety or replace a signature. |

Native build/test commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/edgecourt_tests
node --check web/app.js
node tests/browser_smoke.mjs http://127.0.0.1:3000
ZIG=.cache/python-tools/ziglang/zig bash scripts/build-windows.sh
python3 scripts/package_release.py
```

The sandbox used CMake 3.31.6 and Zig 0.13.0 from its excluded development cache. Python, Node, Chromium and compiler packages are **not** bundled as Windows runtime requirements.

## Coverage highlights

- All bookmaker price displays/manual input/template/export use implied percentages; exact percentage payouts/EV/Kelly and same-book no-vig pairing, plus legacy American-file/provider compatibility.
- American/decimal/implied odds, push-aware expected profit, conditional Kelly with risk cap and Wilson intervals using effective historical sample size.
- All ten requested market types; joint empirical player draws preserve PRA/DD/TD relationships. DD/TD require steals and blocks too.
- Chronological player histories, latest dated team assignment, valid dates, future-history exclusion, minimum sample sizes and team Elo ordering.
- Missing prices are not invented. Missing/null raw statistics are not silently replaced by zero.
- Stale quotes, stale live scores/boxes, passed starts without a confirmed live state, preseason rotations, missing pace/ratings and reported-out players suppress evaluation.
- First-basket field normalization, ten game-scoped confirmed starters, explicit first-FG history, contradictory out starters and already-started events.
- Already-achieved live thresholds, unconfirmed final/overtime transitions and unmodeled player overtime rotations do not produce new edges. Expired forecasts clear sizing fields.
- Atomic CSV/JSON imports, decimal/odds validation, quoting/escaped commas, duplicate IDs, canonical quote replacement and preserved original observation timestamps. An untouched CSV template has no price and cannot manufacture a quote.
- Public-source HTTPS allowlist, response/body limits, NBA/ESPN fixtures, constrained saved-HTML extraction and ESPN team-ID retention.
- Native HTTP assets/routes, write token, bad Host/Origin rejection, Arena preview hostname acceptance, no iframe block and no wildcard CORS.
- Browser market/search/EV/game/status filters, watchlist, research drawer, model lab, manual/file imports, error feedback, HTML escaping/XSS check, CSV formula escaping, export, pagination, settings and responsive mobile category access.
- Synthetic historical-prediction scoring example runs successfully; its favorable or unfavorable metrics are **not model performance evidence**.

The screenshot in `dashboard.png` shows the native browser build in **synthetic Demo mode**. No pictured game, statistic or book price should be interpreted as current NBA information.

## Not established

1. **No Windows runtime execution:** the final executable was cross-compiled and inspected, but has not been run on a Windows 10/11 machine here. Default-browser launch, WinHTTP/provider access and Windows-specific behavior still need an actual Windows smoke test.
2. **No current live-provider verification:** sandbox outbound restrictions prevent testing current NBA/ESPN responses. Fixture compatibility does not prove that undocumented APIs, their schemas or prices remain available from the user's network.
3. **No historical corpus installed or all-history crawl executed:** direct Basketball Reference collection is restricted in the sandbox. The optional collector/model connection is tested with generated synthetic HTML. Current provider schema, source completeness and permission for the user's intended data/model use are not established. Raw data is not committed or packaged. The app's uninstalled/partial coverage state is explicit.
4. **No reliable all-books player-prop feed is bundled.** Statistical inputs and explicit public moneylines have adapters; player-prop/other unavailable prices need user imports. Prices marked retrieved/indicative are not independently verified executable bookmaker offers.
5. **No calibrated/profitable model:** there is no chronologically held-out calibration, model-trained historical backtest, independently measured accuracy, ROI study, executable-price validation or closing-line-value evidence. Monte Carlo draws are not independent real games. Sampling intervals exclude model misspecification.
6. **No Windows CI run was observed.** The repository contains a Windows/Linux build-and-test workflow for subsequent execution; its existence is not a passing check.
7. **Unsigned binary and no formal security audit.** Desktop defaults to loopback. The preview is a development demo, not a public multi-user service. Do not disable OS protections; inspect/build the source if desired.

The model is an explicit experimental research baseline. Positive **Model EV** is conditional on its unvalidated assumptions and the supplied quote, not a verified betting edge or a recommendation to stake money.
