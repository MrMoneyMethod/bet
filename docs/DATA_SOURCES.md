# Public data, limits and exchange activity

Current provider decisions, tests and measured probe results are in [API_AUDIT.md](API_AUDIT.md).
**Live provider functionality is not verified in this network-restricted sandbox.** Fixture validation is not a successful upstream request.

No credentials are required for the core public feeds or exchange activity. Optional API-Basketball, SharpAPI and TheSportsDB V2 use `API_BASKETBALL_KEY`, `SHARPAPI_KEY` and `THESPORTSDB_KEY` from the native process environment, attached only to exact provider hosts in headers. Keys never go in URLs, browser code, reports or caches. Keyless access is not unlimited access or a redistribution license.

## Active providers

| Source | Request family / purpose | App request policy |
|---|---|---|
| ESPN site API | Daily NBA scoreboard: IDs, teams, schedule, scores, clock, season type, records, venue and explicit moneylines when present. No inferred player prices. | Yesterday through next seven UTC days. Today's day caches 45s, other days 1h; no stale score fallback. Daily URLs only, no previously rejected hyphenated date range. |
| ESPN site API | `/teams/{id}/roster`: injury/roster context. `/summary?event={id}`: accrued live player boxes and published odds. | Rosters 1h, at most 10 network roster calls/refresh. Live summaries 30s, at most six/refresh. |
| NBA CDN | `liveData/scoreboard/todaysScoreboard_00.json`: official game IDs, scores, clocks. `liveData/odds/odds_todaysGames.json`: named-book `2way` moneylines, decimal prices converted to American. | 30s scoreboard / 120s odds. Matched official game/team IDs only. Book last-move times are not supplied. |
| NBA CDN | `staticData/scheduleLeagueV2.json`: full season metadata, arenas, broadcasters and schedule context. | 1h cache, bounded stale fallback; enriches matched window games, not a second independent betting schedule. |
| NBA Stats | League-wide player/team game logs, advanced ratings, standings, season averages, Four Factors, player index, estimated metrics, clutch, hustle, tracking shooting/defense, opponent shots and top lineups. | 6h cache, at most 48h stale fallback. One league-wide request per dataset; no per-player fan-out. Recent logs limited to 60 games; 210-day window. Previous season is used only after a valid empty current-season response, not on network failure. |
| NBA Stats static | Daily lineup file and player-movement archive. | 5m lineups / 6h movements. Only explicit confirmed starters can satisfy first-basket guards; projected lineups and transactions remain context. |
| Polymarket Gamma | `/events?tag_slug=nba&active=true&closed=false`: matched NBA game winner prices, **market-level** total/24h volume and liquidity. | 60s cache, 5m maximum stale fallback, at most three pages of 100 events. Retain condition IDs and outcome-token crosswalk for public trades. |
| Polymarket Data API | `/trades`: public taker wallet fills matched by condition and asset, minimum cash notional 1,000 USDC. | 60s cache / 5m stale fallback. Up to 40 markets, two batches of 20, at most two 500-row pages/batch. Retain at most 1,000 observed fills within 24h. Partial coverage explicitly labeled. |
| Kalshi | `/trade-api/v2/markets?series_ticker=KXNBAGAME&status=open`: indicative midpoint/last prices, total/24h **contract** volume and open interest. | 60s cache / 5m stale fallback, at most three 200-market pages. Match structured team tickers plus occurrence time or unique Eastern game-date ticker. No public wallet identities. |
| Pinnacle guest | Public-site basketball leagues, matched NBA matchups and `/markets/straight`. Indicative American moneylines only. | Leagues 10m, matchups 2m, prices 45s; bounded stale metadata. Published guest header, no account login. Obsolete `/period/0` fallback removed; no alternate route after denial. Undocumented, not live-verified here. |
| API-Basketball (optional key) | NBA games crosswalk and full-game Home/Away pregame moneylines. | Daily `/status` quota check, 20-request reserve, 24h schedule/odds cache, 24h failure defer. Documented odds update once daily. Only exact team/time matches. |
| SharpAPI (optional key) | `/api/v1/odds?league=nba&market=moneyline&is_live=false`: pregame book odds. | 5m cache, four-page maximum. Previously inspected free-plan docs specify 12/min and 60s delay; actual account limits not verified. Direct exchange duplicates excluded. |
| NBA_Go | Same-day box, line-score and leaders context, joined by teams/date. | 6h cache, inspected Swagger specifies no key. Does not overwrite canonical score or model inputs. |
| TheSportsDB V2 (optional premium key) | `/api/v2/json/schedule/next/league/4387`: matched schedule/venue context. | 6h cache. Header key only; V1 key-in-URL format is not used. |

## Removed integrations

- **Sportsbook API2:** inferred RapidAPI product host and undocumented `payout` units made the configured moneyline adapter speculative. Parser, request/quota code, host allowlist and credential attachment removed. Setting old RapidAPI environment variables does not reactivate it.
- **SportScore:** repository documentation did not establish the configured general NBA fixtures route or response schema. Speculative parser, request code, allowlisted host and key attachment removed.

Explanatory retired source cards remain. This decision is not a claim the providers themselves are out of business or unavailable; it concerns the adapters' unverified contracts.

## Meaning of volume and whales

The **Volume & whales** view is public exchange context, not sportsbook handle, betting splits, total bet counts, private bettor tracking or a trading signal. Polymarket uses USDC; Kalshi counts contracts. Never add these units together. Volume is stored once per market, not once per outcome. Liquidity and open interest remain separate. Missing values are null, not zero.

Whale thresholds filter observed public fills, default 10,000 USDC. Notional is shares × price; wallet buy/sell sums are turnover in the filtered sample, not profit or net positions. Public wallets are not necessarily unique people. The sample is bounded, may miss fills and expires after 24h. Market metadata expires to a visible stale state after five minutes. Demo is visibly synthetic and isolated from Live. No exchange activity affects model probabilities, EV or sizing.

## Odds, history and saved HTML

- Public odds are indicative; retrieval time is not a bookmaker's last-move time. Confirm price, availability, fees and settlement independently.
- No all-books prop prices are manufactured. Player props and team totals still use explicit local CSV/JSON/manual quote imports. Moneyline is the only live price mapping; game totals are not team totals.
- The optional local historical pipeline is documented in [HISTORICAL_DATA.md](HISTORICAL_DATA.md). Raw all-season data is not bundled or loaded into the browser.
- Saved ESPN HTML requires the original observed price timestamp; importing an old file cannot make its prices fresh. The fixed public ESPN scoreboard HTML fallback is not attempted after network/403/429 denial. It only extracts embedded JSON; never executes JavaScript or crawls arbitrary URLs.
- Basketball Reference game-log HTML imports read tables as data. Online collection is permission-gated; no protected bookmaker scraping, login, CAPTCHA, proxy evasion or challenge bypass is implemented.

## Transport controls

HTTPS fixed-host GETs only, no redirects/user-supplied URLs. Native WinHTTP on Windows; exec'd curl with private header pipe on Linux. 16 MB response cap, 4 MB import cap, **72 requests/refresh** shared budget and 250ms minimum network pacing. Source caches reduce requests; all JSON failures have persisted backoff. 403/429 or repeated transport failures stop host fan-out through persisted circuit breakers. Current price/score envelopes reject malformed/error JSON before caching; context-specific parser guards and per-parse rollback contain exceptions. Stale cache fallback preserves original timestamps. See [API_AUDIT.md](API_AUDIT.md) for known limitations, including lack of dynamic rate-limit header handling.

Public-source failures are visible in **Model lab → Data sources** and never cause a fallback to synthetic Demo. `--live --offline` uses only local imports/history. The development preview runs offline Demo because provider hosts are blocked in this environment. The reference software licenses do not grant rights to redistribute provider datasets; keep caches local and respect terms/attribution.
