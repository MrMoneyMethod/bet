# Free, keyless NBA data sources — catalog

Researched and live-probed on **2026-10-13** (NBA preseason). "Keyless" means readable
with no account, API key, login, or payment. Every entry says how it was checked.
Edgecourt adds sources conservatively: a price is only used when it matches a game
already in the schedule snapshot, missing data stays missing, and exchange prices are
labeled indicative (fees, liquidity and settlement rules differ from sportsbooks).

## Integrated and verified keyless (live-probed)

| Source | What it gives | Access notes | Status in Edgecourt |
|---|---|---|---|
| ESPN site API `site.api.espn.com/apis/site/v2/sports/basketball/nba` | Schedule (preseason, regular season and postseason as published), scores, moneylines when explicitly published, summaries, injuries, rosters | Public undocumented JSON; scoreboard fetched one day at a time because hyphenated date ranges now return HTTP 400; 403/429 stop fallback scraping | Integrated |
| NBA CDN `cdn.nba.com/static/json` | Schedule, live box scores, live play-by-play | Public static JSON | Integrated |
| NBA CDN odds file `cdn.nba.com/static/json/liveData/odds/odds_todaysGames.json` | League-published Sportradar odds: moneyline (`2way`) prices from multiple named sportsbooks (decimal strings), keyed by official gameId + NBA team ids | Public static JSON; named books are the league's own published odds partners | **Integrated: moneylines**, book name preserved per sportsbook (default source `nba_cdn_odds`), matched by NBA game/team ids with a team-id fallback. Spread/total markets in the file are not imported |
| stats.nba.com | Team/player game logs, advanced + Four Factors team stats, standings, player season stats, rosters, PlayerIndex, clutch (team/player), hustle (team/player), estimated metrics (team/player), tracking shots/defense, opponent shots, top lineups (endpoint parameters re-derived from swar/nba_api, MIT) | Keyless but needs browser-like headers; rate-limit/timeout prone | Integrated (all context; see DATA_SOURCES.md) |
| NBA.com CDN + stats.nba.com static JSON | Full-season `scheduleLeagueV2.json`, daily `00_daily_lineups_YYYYMMDD.json`, `NBA_Player_Movement.json` (catalogued in JovaniPink/awesome-nba-data, Official & League Data) | Keyless static files; daily-lineups file may 404 before lineups post | Integrated: schedule enriches existing games only; only explicitly Confirmed daily-lineup starters satisfy the first-basket guard; transactions are dated context, never eligibility |
| NBA Official injury report | Timestamped PDFs from the league injury-report page | No stable bulk API; anti-automation terms | Reviewed only — listed in Model lab as import-required, never scraped or guessed |
| **Polymarket Gamma API** `gamma-api.polymarket.com` | NBA events/markets; `outcomePrices` are traded probabilities 0–1 | Fully public read API (CLOB `clob.polymarket.com` market data and `data-api.polymarket.com` also public; trading needs a wallet). Gamma `/events` is polled at most once/minute, far below its documented 500-request/10-second endpoint cap ([rate limits](https://docs.polymarket.com/quickstart/introduction/rate-limits)) | **Integrated: game moneylines** as native % (`price × 100`), book "Polymarket". Futures/spreads/totals ignored. During preseason only futures were live; game markets are matched defensively by team names + date window |
| **Kalshi trade API v2** `external-api.kalshi.com/trade-api/v2` | Public market data; NBA games in series `KXNBAGAME` | Market data needs no auth; trading needs RSA keys. Prices are dollar strings (`yes_bid_dollars`, …). Public market list is polled at most once/minute; Kalshi documents token-bucket read limits that vary by account tier ([rate limits](https://docs.kalshi.com/getting_started/rate_limits)). The app does not call account or trading endpoints | **Integrated: game moneylines**, % = bid/ask midpoint (else last trade) × 100, book "Kalshi". Team names in titles can be truncated, so matching uses the structured event/market tickers + date |
| **Pinnacle guest Arcadia API** `guest.api.arcadia.pinnacle.com/0.1` | The public feed behind Pinnacle's website: leagues, matchups, straight markets | Guest key model: requests carry the `X-API-Key` Pinnacle publishes to its public website (config/app.json, re-verified 2026-10-13 — still the long-standing published key) plus an anonymous per-process `X-Device-UUID` like the site's own client; keyless calls return 401/404 since Pinnacle tightened the guest tier. Undocumented, may rotate, block, or geo-restrict. Basketball sport id `4`; straight markets live at `/leagues/{id}/markets/straight` (the `/markets/straight/period/0` route answers 404 and remains only as a fallback); market keys `s;0;m` (moneyline), `s;0;s`, `s;0;ou`. The source card names the failing stage (league list / matchup list / straight markets) instead of a bare HTTP status | **Integrated: moneylines**, book "Pinnacle". Spreads exist; game totals are *not* team totals and are ignored |
| ESPN core API `sports.core.api.espn.com/v2/.../events/{id}/competitions/{id}/odds` | DraftKings (provider 100) open/close/current lines, plus a `propBets` sub-endpoint | Keyless. Verified live: team-total prop bets returned (`type` "Team Total Points", `odds.american.value`, `current.target`) | **Not yet integrated** — player-prop item shape and over/under side encoding were not verifiable in preseason; the highest-value research lead for keyless DraftKings prices. (The `/odds-providers` index path returns 404 — do not use it.) |

## Keyless open datasets (download, don't scrape)

| Dataset | Coverage | Notes |
|---|---|---|
| **SportsDataverse / hoopR releases** (`github.com/sportsdataverse/sportsdataverse-data`) | ESPN play-by-play 2002–present (`espn_nba_pbp`: `play_by_play_{season}.csv/.parquet/.rds`), player/team box scores, schedules, standings, rosters, officials, plus `nba_stats_*` releases (hustle, synergy, matchups…). 120M+ rows claimed | **Now automatic in the app**: the live app downloads the last five seasons itself in the background and builds the feature bundle natively (`src/history_download.cpp`; see HISTORICAL_DATA.md). The Python `scripts/basketball_reference_history.py hoopr --player-box-csv … --pbp-csv … --season YYYY` path still works and builds the same bundle as the Basketball-Reference pipeline. GitHub *release assets* must be downloaded on your machine (`gh release download espn_nba_pbp --repo sportsdataverse/sportsdataverse-data`). Column aliases are checked loudly — unknown headers fail with a listing, never a guess. This sandbox cannot download release assets, so the converter is covered by synthetic fixtures matching the hoopR schema |
| hoopR package sources (`stats.nba.com` wrappers, NBA CDN live PBP, data.nba.com PBP 2016-17→) | Function catalogs document more keyless endpoints | hoopR itself is R; Edgecourt re-derives the same endpoints in C++/Python where useful |
| FiveThirtyEight NBA Elo (`github.com/fivethirtyeight/data` → `nba-elo/nbaallelo.csv`) | Historical Elo game file | Keyless CSV in-repo; project era has ended — verify freshness before using as a feature |
| hoopR-data games index (`nba/nba_games_in_data_repo.csv`) | Game-id crosswalk | Keyless in-repo CSV |

## Optional keyed sources (integrated)

- **API-Basketball / API-Sports** — optional NBA game moneylines, enabled only by the user's `API_BASKETBALL_KEY` process-environment variable. It sends the key in an `x-apisports-key` header only to the fixed `v1.basketball.api-sports.io` host; no key is bundled, persisted, logged or exposed to the browser. The adapter checks `/status` before its first billed request each UTC day, reads the provider's daily quota, tracks subsequent attempts locally, and protects a 20-request reserve. The documented Free tier has 100 requests/day (reset 00:00 UTC), `/status` is not billed against that daily quota, and odds update once/day. Schedule/odds responses cache 24 hours; failures are not retried more than daily. Parser and host/key isolation are fixture-tested, **not live-probed in this sandbox**; see [DATA_SOURCES.md](DATA_SOURCES.md).
- **SharpAPI** — optional NBA pregame sportsbook moneylines using `SHARPAPI_KEY` in an `X-API-Key` header to the exact `api.sharpapi.io` host. The documented Free tier includes DraftKings and FanDuel, 12 requests/minute and a 60-second data delay; no daily cap is published. The authenticated [`/account/usage`](https://docs.sharpapi.io/en/api-reference/account-usage/) endpoint documents daily/weekly/monthly request totals and per-minute remaining/reset, but Edgecourt does not call it; no account-specific usage was checked. Docs publish `X-RateLimit-*` headers and recommend exponential backoff on 429s; Edgecourt currently defers failed pages for five minutes without dynamic header/`retry_after` handling. It polls pages at most once per 5 minutes and follows at most 4 cursor pages. Direct Polymarket/Kalshi quotes remain on their existing keyless adapters. Fixture-tested only; no live request or 429 probe was made. See [DATA_SOURCES.md](DATA_SOURCES.md).

## User-suggested sources: integration status

- **Sportsbook API2** ([RapidAPI product](https://rapidapi.com/sportsbook-api-sportsbook-api-default/api/sportsbook-api2), [provider site](https://sportsbookapi.com/), [Swagger/OpenAPI](https://api.sportsbookapi.com/documentation/static/index.html)) — integrated as an optional named-book pregame NBA moneyline source. The OpenAPI routes discover the NBA competition, list events over the app's schedule window, then retrieve latest outcomes for matched event keys. Set `RAPIDAPI_KEY`; it is sent only as `X-RapidAPI-Key` to the configured `sportsbook-api2.p.rapidapi.com` host (inferred from the product slug). Compare that host with the generated RapidAPI code snippet before first live use; it was not confirmed by a provider call here. Set non-secret `RAPIDAPI_HOST` to that exact value only after the comparison; otherwise the app sends no request and no key. The key is never in the URL. Since the provider site says 50 requests/day and RapidAPI says 150/day, Edgecourt honors 50/day, uses at most 40/day and reserves ten. It caches the competition/events/odds stages for 24h/6h/2h. Numeric `payout` has no documented unit: parser supports American-looking values and plausible decimal values with an explicit heuristic, which still needs a sanitized live fixture. **No authenticated request has been made.**
- **Broadage Sports Basketball Data** ([RapidAPI product](https://rapidapi.com/BroadageSports/api/basketball-data), [provider coverage](https://www.broadage.com/sports-data-api/basketball)) — free Basic is listed as 500,000 requests/month and 1000/hour. The public endpoint catalog covers schedules/fixtures, live scores, play-by-play, box scores, match stats, standings and player data; it does not list sportsbook odds. Not integrated because that overlaps existing ESPN/NBA data and does not add the requested prices. No live request was made.
- **Free NBA** ([RapidAPI product](https://rapidapi.com/theapiguy/api/free-nba)) — a free Basic historical-data API with player/team/game/stat endpoints; no sportsbook odds. Its readable product page lists HTTP 429 but no numeric request cap, and the pricing page did not expose a more specific quota. Not integrated because it would duplicate existing NBA/ESPN statistical sources. No live request was made.
- **NBA_Go** ([OpenAPI](https://api.server.nbaapi.com/swagger/doc.json), [Postman collection](https://documenter.getpostman.com/view/25652688/2sB34Zs4xZ)) — integrated for same-Eastern-date game/box-score/player/team context. The inspected Swagger documents public GET routes but neither authentication nor a request quota, so the app sends no key and polls at most every six hours when a game is scheduled that day. Its data is attached as context only, not used to alter canonical scores or probabilities. No live request was made; current availability/schema remains unverified.
- **TheSportsDB** ([docs](https://www.thesportsdb.com/documentation)) — integrated with the premium-only V2 NBA next-events route (`league/4387`) for schedule/venue/status context. Set `THESPORTSDB_KEY`; only the documented `X-API-KEY` header is used. V1 is intentionally not used because its API key is in the URL. The documented premium rate is 100 requests/minute; Edgecourt caches for six hours (at most four polls/day). Context only; no live request was made.
- **SportScore** ([developer docs](https://sportscore.com/developers/), [API reference](https://sportscore.com/developers/api/)) — adapter configured for a general fixtures route to supply matched NBA schedule/score context, not odds or model inputs. The examined reference documents team-scoped schedules and competitions/players but did not establish this general fixtures route or its response schema; NBA coverage remains unconfirmed until a sanitized live response is checked. Public tier is keyless and documents about 10,000 requests/24h/IP; optional `SPORTSCORE_KEY` is sent only via `X-Api-Key`. Edgecourt polls no more than every six hours (four/day). The terms require a visible dofollow “Powered by SportScore” link wherever its data is shown; the app renders that in the source card and research panel. No live request was made.

## Free key required — explicitly *not* integrated

- **balldontlie** — current docs require an `Authorization` API key on every request (free tier exists). Older "no key needed" claims are stale; do not integrate as keyless.
- **The Odds API** — free tier needs a key; it is not configured.
- **SportsDataIO, Sportradar, etc.** — paid/keyed, out of scope.

## Documented keyless elsewhere — not probed, not integrated

- **Kambi offering API** (`eu-offering-api.kambicdn.com/offering/v2018/{operator}/…`) — powers Unibet/BetRivers price feeds; operator-specific paths, unverified live here.
- **Bovada public site endpoints** — undocumented, geo/login sensitive; treated as scrape-gray, not integrated.
- **sportsdata-mcp provider notes** — a catalog of ~44 keyless provider APIs (Polymarket, Kalshi, Pinnacle, Kambi among them); useful discovery index for future adapters.
- **pbpstats** — valuable lineup/possession data; its API token requirements were not verified as keyless, so it stays out until proven.

## Dead ends (checked 2026-10-13)

- **DraftKings public API** — none exists (confirmed by multiple current sources). Direct use of DraftKings' internal site endpoints would violate their terms; the keyless route to DraftKings prices is ESPN's core odds (above).
- **data.nba.net** — reported dead/unreliable; NBA moved to the CDN endpoints already integrated.
- **ESPN `/odds-providers`** — 404 application error.
- **Kaggle NBA datasets** — account required (not keyless) and frequently stale.

## Scrape-with-permission (already shipped)

- **Basketball-Reference** — the complete historical PBP archive pipeline lives in
  `scripts/basketball_reference_history.py` (see `docs/HISTORICAL_DATA.md`). Sports
  Reference's terms prohibit automated access/tool-building without permission and
  the site rate-limits aggressively (robots crawl-delay 3s; >20 requests/min gets an
  IP jailed), so crawling is permission-gated and the hoopR dataset above is the
  keyless historical alternative.

## Standing rules for any future source

1. Keyless adapters never request credentials; an explicitly optional keyed adapter
   must keep its user-provided key out of source, browser code, URLs, logs and caches.
   No login, paid tier or anti-bot/proxy/CAPTCHA workaround is assumed.
2. Prices only join games already in the snapshot (teams + ≤2-day window); unmatched
   markets are dropped, never force-fit. Truncated display names are never matched
   alone — structured tickers/ids are required.
3. Probability-native venues (Polymarket/Kalshi) map price×100 straight onto
   `oddsPercent`; American prices keep their exact integer and convert in one place.
4. Only moneyline maps onto an app market today. Spreads and *game* totals have no
   honest mapping to team totals and are ignored until real markets exist.
5. Every fetch stays inside the per-refresh request budget and source-specific quota,
   with cached fallbacks; every source card states what the data is and is not.
