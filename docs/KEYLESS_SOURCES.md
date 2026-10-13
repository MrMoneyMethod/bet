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
| stats.nba.com | Team/player game logs, advanced + Four Factors team stats, standings, player season stats, rosters, PlayerIndex, clutch (team/player), hustle (team/player), estimated metrics (team/player), tracking shots/defense, opponent shots, top lineups (endpoint parameters re-derived from swar/nba_api, MIT) | Keyless but needs browser-like headers; rate-limit/timeout prone | Integrated (all context; see DATA_SOURCES.md) |
| NBA.com CDN + stats.nba.com static JSON | Full-season `scheduleLeagueV2.json`, daily `00_daily_lineups_YYYYMMDD.json`, `NBA_Player_Movement.json` (catalogued in JovaniPink/awesome-nba-data, Official & League Data) | Keyless static files; daily-lineups file may 404 before lineups post | Integrated: schedule enriches existing games only; only explicitly Confirmed daily-lineup starters satisfy the first-basket guard; transactions are dated context, never eligibility |
| NBA Official injury report | Timestamped PDFs from the league injury-report page | No stable bulk API; anti-automation terms | Reviewed only — listed in Model lab as import-required, never scraped or guessed |
| **Polymarket Gamma API** `gamma-api.polymarket.com` | NBA events/markets; `outcomePrices` are traded probabilities 0–1 | Fully public read API (CLOB `clob.polymarket.com` market data and `data-api.polymarket.com` also public; trading needs a wallet) | **Integrated: game moneylines** as native % (`price × 100`), book "Polymarket". Futures/spreads/totals ignored. During preseason only futures were live; game markets are matched defensively by team names + date window |
| **Kalshi trade API v2** `external-api.kalshi.com/trade-api/v2` | Public market data; NBA games in series `KXNBAGAME` | Market data needs no auth; trading needs RSA keys. Prices are dollar strings (`yes_bid_dollars`, …) | **Integrated: game moneylines**, % = bid/ask midpoint (else last trade) × 100, book "Kalshi". Team names in titles can be truncated, so matching uses the structured event/market tickers + date |
| **Pinnacle guest Arcadia API** `guest.api.arcadia.pinnacle.com/0.1` | The public feed behind Pinnacle's website: leagues, matchups, straight markets | Anonymous guest token model; undocumented, may change or block. Basketball sport id `4`; market keys `s;0;m` (moneyline), `s;0;s`, `s;0;ou` | **Integrated: moneylines**, book "Pinnacle". Spreads exist; game totals are *not* team totals and are ignored |
| ESPN core API `sports.core.api.espn.com/v2/.../events/{id}/competitions/{id}/odds` | DraftKings (provider 100) open/close/current lines, plus a `propBets` sub-endpoint | Keyless. Verified live: team-total prop bets returned (`type` "Team Total Points", `odds.american.value`, `current.target`) | **Not yet integrated** — player-prop item shape and over/under side encoding were not verifiable in preseason; the highest-value research lead for keyless DraftKings prices. (The `/odds-providers` index path returns 404 — do not use it.) |
| TheSportsDB `thesportsdb.com/api/v1/json/3` | Teams/leagues/results with the shared public test key `3` | Technically keyed (public test key); NBA league id 4387 | Not integrated (redundant with ESPN/NBA); fallback candidate |

## Keyless open datasets (download, don't scrape)

| Dataset | Coverage | Notes |
|---|---|---|
| **SportsDataverse / hoopR releases** (`github.com/sportsdataverse/sportsdataverse-data`) | ESPN play-by-play 2002–present (`espn_nba_pbp`: `play_by_play_{season}.csv/.parquet/.rds`), player/team box scores, schedules, standings, rosters, officials, plus `nba_stats_*` releases (hustle, synergy, matchups…). 120M+ rows claimed | **Ingest implemented**: `scripts/basketball_reference_history.py hoopr --player-box-csv … --pbp-csv … --season YYYY` builds the same feature bundle as the Basketball-Reference pipeline. GitHub *release assets* must be downloaded on your machine (`gh release download espn_nba_pbp --repo sportsdataverse/sportsdataverse-data`). Column aliases are checked loudly — unknown headers fail with a listing, never a guess. This sandbox cannot download release assets, so the converter is covered by synthetic fixtures matching the hoopR schema |
| hoopR package sources (`stats.nba.com` wrappers, NBA CDN live PBP, data.nba.com PBP 2016-17→) | Function catalogs document more keyless endpoints | hoopR itself is R; Edgecourt re-derives the same endpoints in C++/Python where useful |
| FiveThirtyEight NBA Elo (`github.com/fivethirtyeight/data` → `nba-elo/nbaallelo.csv`) | Historical Elo game file | Keyless CSV in-repo; project era has ended — verify freshness before using as a feature |
| hoopR-data games index (`nba/nba_games_in_data_repo.csv`) | Game-id crosswalk | Keyless in-repo CSV |

## Free key required — explicitly *not* keyless

- **balldontlie** — current docs require an `Authorization` API key on every request (free tier exists). Older "no key needed" claims are stale; do not integrate as keyless.
- **The Odds API** — free tier needs a key. Edgecourt deliberately has no key configuration.
- **API-Basketball (api-sports.io)** — key required.
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

1. No API keys, logins, paid tiers, or anti-bot/proxy/CAPTCHA workarounds — ever.
2. Prices only join games already in the snapshot (teams + ≤2-day window); unmatched
   markets are dropped, never force-fit. Truncated display names are never matched
   alone — structured tickers/ids are required.
3. Probability-native venues (Polymarket/Kalshi) map price×100 straight onto
   `oddsPercent`; American prices keep their exact integer and convert in one place.
4. Only moneyline maps onto an app market today. Spreads and *game* totals have no
   honest mapping to team totals and are ignored until real markets exist.
5. Every fetch stays inside the per-refresh request budget with cached fallbacks,
   and every source card states what the data is and is not.
