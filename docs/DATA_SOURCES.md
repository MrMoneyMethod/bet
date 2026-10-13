# Public data, access limits and scraping policy

No API keys or sportsbook credentials are requested. Keyless public access is not a contractual guarantee and not a license to redistribute data.

## Bundled adapters

| Source | Request family | Inputs | Cache / fallback |
|---|---|---|---|
| ESPN site API | `https://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard?dates=YYYYMMDD&limit=100`, one request per day | Yesterday through next 7 days: event IDs, teams, schedules, season type (preseason / regular / postseason kept as published), scores, clock, venue, record, explicit moneyline fields when present. Hyphenated `dates=YYYYMMDD-YYYYMMDD` ranges now return HTTP 400, so they are never requested | 45 seconds per day; no stale scoreboard fallback; a day that fails is reported as a partial source, never backfilled |
| ESPN public HTML fallback | `https://www.espn.com/nba/scoreboard` | Embedded event JSON / explicit moneylines; current day only | Only when no daily JSON response follows the known contract, never after access/rate-limit denial (403/429) |
| NBA CDN | `https://cdn.nba.com/static/json/liveData/scoreboard/todaysScoreboard_00.json` | Today's live scores, period, clock, NBA game ID | 30 seconds; no stale fallback; matches merged with ESPN by teams/start date |
| NBA stats | `https://stats.nba.com/stats/playergamelogs?...` | Recent player minutes, points, rebounds, assists, threes, steals, blocks, shooting volume/efficiency, turnovers, fouls, offensive rebounds | 6 hours; at most 48-hour explicit historical cache fallback |
| NBA stats | `https://stats.nba.com/stats/leaguedashteamstats?...MeasureType=Advanced...` | Team pace, offense/defense/net rating | 6 hours; at most 48-hour cache fallback |
| NBA stats | `https://stats.nba.com/stats/leaguegamelog?...PlayerOrTeam=T...` | Paired chronological team results, point/margin variance, windowed Elo, rest dates | 6 hours; at most 48-hour cache fallback |
| NBA stats (via swar/nba_api endpoint definitions) | `https://stats.nba.com/stats/leaguestandingsv3?...` | League standings: W/L, win %, home/road, last-10, streak, conference rank, games back, points for/against per game. **Context only** — shown on schedule cards and in moneyline research, never a probability input | 6 hours; at most 48-hour cache fallback |
| NBA stats (via swar/nba_api endpoint definitions) | `https://stats.nba.com/stats/leaguedashplayerstats?...MeasureType=Base&PerMode=PerGame...` | Season per-game player averages (PTS/REB/AST/3PM, shooting %, plus-minus, DD2/TD3 counts) enriching players who already have dated game logs. **Context only**; a season line never creates a player or replaces logs | 6 hours; at most 48-hour cache fallback |
| NBA stats (via swar/nba_api endpoint definitions) | `https://stats.nba.com/stats/leaguedashteamstats?...MeasureType=Four%20Factors...` | Team Four Factors: effective FG%, free-throw rate, turnover %, offensive-rebound % (and opponent values). **Context only** for moneyline/team-total research | 6 hours; at most 48-hour cache fallback |
| ESPN rosters | `https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/{id}/roster` | Roster/name/team crosswalk and listed injury context | 1 hour; at most 2-hour cache fallback |
| ESPN summary | `https://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event={id}` | Live player minutes/accrued stats, starter flags, pick-center explicit moneylines when present | 30 seconds; no stale live fallback |
| Optional authorized Basketball Reference history | Local SQLite archive and streaming feature export; historical HTTPS collection is separate from the app | All parsed timed PBP events, paired box-score vectors, dated first-FG opportunities and approximate team environment fallback | No corpus bundled or full crawl run here. Source permission required; coverage/gaps explicit. See [historical setup](HISTORICAL_DATA.md). |
| User CSV/JSON | Local import only | All requested odds markets; explicit schedules/history/feature overrides | Quote times preserved. Validate accuracy yourself. |
| User saved HTML | Local data extraction | Embedded ESPN/normalized JSON; Basketball Reference-style game-log tables | No browsing, crawling or script execution |
| Polymarket moneylines | gamma-api.polymarket.com (keyless) | Game-winner traded probabilities only; futures/spreads/totals ignored | Indicative exchange price; no liquidity/size guarantee |
| Kalshi moneylines | external-api.kalshi.com trade API (keyless) | NBA game markets matched by structured tickers; bid/ask midpoint else last trade | Indicative exchange price; names in titles truncate, tickers do not |
| Pinnacle moneylines | guest.api.arcadia.pinnacle.com (anonymous guest feed) | Exact American prices; game totals never mapped to team totals | Undocumented endpoint; may change or block |

The full catalog of investigated keyless sources (probed endpoints, open datasets,
free-key exclusions and dead ends) is in [KEYLESS_SOURCES.md](KEYLESS_SOURCES.md).

The NBA player endpoint uses the recent 210-day window; histories are limited to 60 recent games per player. If the current-season player response successfully returns zero rows, the prior regular season is requested. A failed/blocked current-season request is **not** treated as proof that a prior-season endpoint will work. Preseason forecasts remain disabled.

All provider parsers tolerate absent supported fields; missing data remains missing. Schema fixtures cover both legacy `homeTeamOdds.moneyLine` and newer `moneyline.home/away.close.odds` ESPN shapes. The adapters cannot guarantee those fields currently exist at any public source.

## Odds coverage

ESPN public moneyline prices are **indicative**, not independently verified executable prices. Many responses do not include moneylines at all. Retrieval time is not necessarily the book's last-move timestamp.

No all-books prop prices are manufactured from implied model probabilities, game totals, spreads, league averages or demo rows. Team-total odds are not inferred from the game total. A scheduled game without public prices produces explicit unpriced placeholders and zero +EV claims.

Player-prop odds are supplied by local CSV/JSON/manual quote import. All bookmaker prices are displayed as implied percentages including book margin, separate from Model % and paired no-vig percentages. First basket additionally needs current confirmed lineups; dated first-FG opportunities can come from the installed optional historical archive or explicit supplied history. No actual historical corpus is bundled.

## Historical collection

The optional standard-library Python collector discovers completed NBA games through season/month schedules, pairs box scores with complete PBP pages, caches content-hashed originals, persists every parsed timed event and checkpoints progress in SQLite. It is permission gated, checks robots (including wildcard/anchored rules), waits at least 3.1 seconds between requests and stops on access/rate-limit denial without bypasses. It does not run on each live refresh. The C++ app reads bounded exported features and reports coverage. Current provider responses/full historical coverage were **not** verified here. [Collection, permissions and model use](HISTORICAL_DATA.md).

The native historical/model workspace can be opened with `--live --offline` to disable all live public requests, retaining local history/imports and freshness/settlement guards.

## Scraping implemented

A fixed public ESPN scoreboard-page fallback is requested automatically only when no daily JSON response follows the known scoreboard contract. It is **not attempted after 403/429/network denial** and is not a challenge bypass. It extracts embedded data from that one public page, not protected bookmaker pages. The same bounded parser is available for locally saved HTML.

Saved ESPN HTML price imports require an explicit original observation timestamp; file import time is not used to make old prices fresh. The automatic public-page fallback uses the actual HTTPS retrieval timestamp, still labeled indicative rather than a book-last-move time.

The C++ **HTML data scraper** supports:

1. `application/json` script blocks containing normalized Edgecourt input or an ESPN event tree.
2. ESPN's `__espnfitt__` embedded JSON data assignment (balanced JSON scanner, no JS evaluator).
3. Basketball Reference-style player game-log `<tr>` / `<td data-stat="...">` tables. Supply a player name/current team; minutes such as `34:30` become decimal minutes. DNP/incomplete rows are omitted. Basic entity decoding and tag stripping extract only table values.

This is not a browser automation scraper. It does not download arbitrary user URLs, log in, defeat CAPTCHA, use proxies to evade blocks, mimic a logged-in session, inspect protected sportsbook APIs or execute HTML/JavaScript. Obtain a permitted public HTML export yourself and respect its source's terms/robots/access rules. Support for a historical table format is not a claim that the provider currently grants automated access.

The historical SBR reference was reviewed for data/odds separation and parsing patterns; its old archive scraper and historical data are **not** bundled as a current-live sportsbook feed. No protected bookmaker scraping is implemented.

## Transport and rate controls

- HTTPS GET only to a fixed source hostname allowlist; no custom ports, credentials, localhost targets or redirects.
- Descriptive application user agent; Windows certificate validation remains enabled.
- Timeouts, 16 MB source-response cap, 4 MB imports, 36 network requests per refresh maximum and at least 250 ms between source requests.
- Cache avoids refetching season stats/rosters every score refresh. Within the request budget, the schedule and the public moneyline venues are fetched before player/team stats, live summaries and rosters, so enrichment traffic cannot starve prices.
- Failed/blocked/rate-limited sources are surfaced in **Model lab → Data sources**. The code does not retry through bypasses.
- Desktop public-data requests originate on your computer, not in browser JavaScript, avoiding provider CORS requirements.
- Model price/stat freshness is stricter than cache availability. A cached history response does not make an old price fresh.

The reference/source software licenses do not grant NBA, ESPN, Basketball Reference or sportsbook data redistribution rights. Local caches are personal research artifacts, not committed datasets. Clear/delete your own cache if necessary; preserve any source attribution required by the provider.

## Sandbox limitation

The development environment allows outbound access only to GitHub/npm/PyPI hosts, so NBA/ESPN providers and a full Basketball Reference crawl cannot be live-verified here. The default preview is **clearly synthetic**. Public endpoint schemas were translated from inspected GitHub reference definitions and fixture-tested. Test actual feed availability on Windows before assuming any quote/history coverage.
