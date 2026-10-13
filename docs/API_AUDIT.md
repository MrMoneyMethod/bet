# Provider audit and exchange activity — 2026-10-13

## What was actually verified

- The native application builds with GCC 12 / CMake (Release).
- All five CTest suites pass: native core/feed regression, exchange activity/transport integration, Python history, native history smoke, and HTTP API smoke.
- Both headless browser suites pass against the native **offline** application: the original UI suite and `tests/browser_activity.mjs` (filters, export, light/dark/mobile rendering, empty states, Demo/Live isolation).
- The native provider probe was run. **No provider returned an HTTP response in this sandbox**: outbound HTTPS to sports/exchange hosts is restricted. See [provider-probe.json](provider-probe.json) for actual statuses and errors, including requests suppressed by the host circuit.
- Fixture tests verify parser behavior and the real refresh/cache pipeline using an injected transport. They do **not** verify current upstream schemas, credentials, coverage, provider uptime or executable prices.
- Exchange activity and mocked refresh/cache tests also pass with GCC UndefinedBehaviorSanitizer enabled (`-fsanitize=undefined -fno-sanitize-recover=all`).
- Windows release artifacts were subsequently rebuilt on this branch with Zig 0.13.0 (x86_64-windows-gnu), including the new activity module and embedded UI. PE architecture, DLL imports, ZIP integrity and SHA-256 manifests were inspected; the binary has **not** been runtime-tested on Windows here.

The review focused on the first-party network, parsing, state, model-isolation and UI paths affected by this change. Vendored `nlohmann/json` and `cpp-httplib` headers were not independently re-audited. This is not an exhaustive security certification or a claim that every upstream endpoint is working.

## Provider choices

Choose bulk, keyless data first; do not equate “no published quota” with unlimited use. No endpoint is stress-tested to discover a maximum rate.

| Provider | Decision | Rationale / limits enforced by Edgecourt |
|---|---|---|
| ESPN | Keep primary schedule/roster/box scores | Multiple useful fields per scoreboard. Today's UTC day caches 45s; other days 1h (previously all nine days refreshed every minute). Rosters 1h, live summaries 30s. Undocumented service; no guaranteed quota. |
| NBA CDN | Keep official scoreboard / odds / schedule | Public bulk files, no per-player request fan-out; caches 30s / 120s / 1h. No guaranteed quota. |
| NBA Stats | Keep league-wide logs and context | Whole-league responses cached 6h, bounded historical fallback. Two transport failures stop further requests to the host for at least 5m instead of repeatedly timing out on each endpoint. |
| Polymarket Gamma | Keep and expand | NBA game prices **and market volume/liquidity in the same response**, at most three 100-event pages per minute. A capped/failed page is reported as partial. |
| Polymarket Data API | Add public trade sample | `/trades`, validated condition-ID batches, `takerOnly=true`, `filterType=CASH`, `filterAmount=1000`. At most 40 matched markets, batches of 20; two 500-row pages per batch, maximum four requests/minute. Rotate excess market IDs. Only rolling-24h matched fills retained. |
| Kalshi | Keep and expand | NBA prices **and contract volume/open interest in the same response**, at most three 200-market cursor pages/minute. Repeated/oversized cursors stop safely. No public bettor identities. |
| Pinnacle guest | Keep existing indicative adapter; remove obsolete route | Remove `/markets/straight/period/0` fallback. A denied request must not trigger alternate routes. Guest service is undocumented and not verified here. |
| SharpAPI / API-Basketball | Keep opt-in | Existing explicit-key, exact-host and quota rules retained. They are not required for volume or whales. Keyed responses not live-tested. |
| NBA_Go / TheSportsDB | Keep context adapters | NBA_Go's inspected schema specifies no key; TheSportsDB V2 is optional/premium. Six-hour cache; no influence on model probabilities. |
| Sportsbook API2 | **Remove** | Configured RapidAPI host was inferred and `payout` units were guessed. Remove parser, quota code, runtime requests, credentials and allowed host; keep an explanatory retired source card. |
| SportScore | **Remove** | Repository's own source notes say the configured general NBA fixtures route/response contract was not established. Remove speculative parser, runtime requests, credential attachment and allowed host. |

Removal is based on identified contract problems, **not** on this sandbox's network failures. Remaining optional or undocumented providers are not being represented as live-verified. There is no defensible global “most data / least limits” winner without usable current responses and account-specific plans.

## Failure behavior

- Shared cap stays **72 network requests per refresh**; cache hits are free. Actual network requests are spaced at least 250ms apart.
- Every JSON feed has a persisted failure defer. Repeated URL failures exponentially back off (60s base; 403/429 base 15m; 404 6h; up to 6h, or a longer provider-specific interval).
- 403/429 immediately open a host circuit. Two transport failures open one for at least 5m. These records survive restart. No alternate-host/route/challenge bypass.
- HTTP 200 is not automatically success: error envelopes and wrong top-level shapes are rejected before caching. Known price/score feeds use explicit envelopes; older context adapters additionally rely on their parser guards.
- Parser exceptions roll back that parse's mutations rather than leaving partial data mislabeled as rejected. Previously accepted pages may remain, labeled on the source card.
- Successful and stale cache reads preserve the original observation time. Retry/refresh does not make an old price, volume or trade newly observed.
- The current transport does **not** consume `Retry-After` / dynamic rate-limit headers. Backoff is conservative and fixed-policy, not a guarantee of compliance with every account-specific limit.

## Volume and whale semantics

`GET /api/state` now includes `activity`:

- `markets`: unique `provider:marketId` records with `gameId`, label, total `volume`, provider-reported `volume24h`, `liquidity`, `openInterest`, `unit`, `observedAt`, and request-time `stale`.
- `trades`: validated Polymarket public wallet fills, transaction hash, asset, selection/team, BUY/SELL, shares, price, `notional = shares × price`, trade time, original retrieval time, and a deterministic fill identity.
- `coverage`: sampled/partial/unavailable/demo state, observed time, request/page counts, market counts, cap/failure indicators and explicit limitations.
- `minimumTradeNotional = 1000`, `windowHours = 24`; the UI defaults to a 10,000-USDC threshold.

**USDC volume and Kalshi contracts are different units and are never added together.** Gamma market volume is recorded once, not twice for home/away prices and not from parent-event volume that could include totals/spreads. Unknown, negative, malformed or unavailable values remain null, not zero. Open interest and liquidity are not volume. Five-minute-old market observations are marked stale and excluded from fresh summary totals.

Trade joins require a validated 32-byte condition ID and outcome-token mapping from a matched NBA moneyline market. Public wallet/transaction hashes, side, numeric ranges and timestamps are validated. Trade titles and claimed outcome labels cannot override the validated token selection. Future/older-than-24h/small/unmatched fills are rejected. Exact duplicate rows across pages and refreshes are collapsed; distinct fills in the same transaction survive. Without a provider fill index, identical same-transaction fills may be indistinguishable—this is **not an accounting ledger**.

At most 1,000 observed fills are kept in memory; provider page caches are local. Restart may lose retained observations outside the newly fetched pages. Closed/disappeared markets may stop receiving fresh fills. There is no completeness guarantee, inferred sportsbook handle, private bettor tracking, net position/profit inference, or claim that a large wallet is informed. Activity does not affect model probability, EV or stake sizing. Demo records are explicitly synthetic and cannot carry into Live.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
# On a network permitting the provider domains. Exit 1 means failed/unverified probes.
./build/edgecourt_probe --live > provider-probe.json
# Local offline browser test target:
./build/Edgecourt --demo --offline --no-browser --port 3000
# In another terminal, after installing the optional browser dependencies:
node tests/browser_smoke.mjs http://127.0.0.1:3000
node tests/browser_activity.mjs http://127.0.0.1:3000
```

Windows multi-config builds put executables under `build/Release/`; use `ctest -C Release`. Never put keys in the probe command, report, browser or Git. Before relying on live data, obtain a schema-valid live response, confirm NBA event/token joins against the provider, and check your provider's current terms and limits.
