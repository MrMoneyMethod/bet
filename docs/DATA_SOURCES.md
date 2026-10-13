# Public data, access limits and scraping policy

No API keys or sportsbook credentials are requested. Keyless public access is not a contractual guarantee and not a license to redistribute data.

## Bundled adapters

| Source | Request family | Inputs | Cache / fallback |
|---|---|---|---|
| ESPN site API | `https://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard?dates=YYYYMMDD-YYYYMMDD&limit=100` | Yesterday through next 7 days: event IDs, teams, schedules, season type, scores, clock, venue, record, explicit moneyline fields when present | 45 seconds; no stale scoreboard fallback |
| ESPN public HTML fallback | `https://www.espn.com/nba/scoreboard` | Embedded event JSON / explicit moneylines; current day only | Only after JSON 404 or a successful unsupported JSON contract, never after access/rate-limit denial |
| NBA CDN | `https://cdn.nba.com/static/json/liveData/scoreboard/todaysScoreboard_00.json` | Today's live scores, period, clock, NBA game ID | 30 seconds; no stale fallback; matches merged with ESPN by teams/start date |
| NBA stats | `https://stats.nba.com/stats/playergamelogs?...` | Recent player minutes, points, rebounds, assists, threes, steals, blocks, shooting volume/efficiency, turnovers, fouls, offensive rebounds | 6 hours; at most 48-hour explicit historical cache fallback |
| NBA stats | `https://stats.nba.com/stats/leaguedashteamstats?...MeasureType=Advanced...` | Team pace, offense/defense/net rating | 6 hours; at most 48-hour cache fallback |
| NBA stats | `https://stats.nba.com/stats/leaguegamelog?...PlayerOrTeam=T...` | Paired chronological team results, point/margin variance, windowed Elo, rest dates | 6 hours; at most 48-hour cache fallback |
| ESPN rosters | `https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/{id}/roster` | Roster/name/team crosswalk and listed injury context | 1 hour; at most 2-hour cache fallback |
| ESPN summary | `https://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event={id}` | Live player minutes/accrued stats, starter flags, pick-center explicit moneylines when present | 30 seconds; no stale live fallback |
| User CSV/JSON | Local import only | All requested odds markets; explicit schedules/history/feature overrides | Quote times preserved. Validate accuracy yourself. |
| User saved HTML | Local data extraction | Embedded ESPN/normalized JSON; Basketball Reference-style game-log tables | No browsing, crawling or script execution |

The NBA player endpoint uses the recent 210-day window; histories are limited to 60 recent games per player. If the current-season player response successfully returns zero rows, the prior regular season is requested. A failed/blocked current-season request is **not** treated as proof that a prior-season endpoint will work. Preseason forecasts remain disabled.

All provider parsers tolerate absent supported fields; missing data remains missing. Schema fixtures cover both legacy `homeTeamOdds.moneyLine` and newer `moneyline.home/away.close.odds` ESPN shapes. The adapters cannot guarantee those fields currently exist at any public source.

## Odds coverage

ESPN public moneyline prices are **indicative**, not independently verified executable prices. Many responses do not include moneylines at all. Retrieval time is not necessarily the book's last-move timestamp.

No all-books prop prices are manufactured from implied model probabilities, game totals, spreads, league averages or demo rows. Team-total odds are not inferred from the game total. A scheduled game without public prices produces explicit unpriced placeholders and zero +EV claims.

Player-prop odds are supplied by local CSV/JSON/manual quote import. First basket additionally needs confirmed lineups and historical first-FG counts, which are not bundled as an automatic corpus.

## Scraping implemented

A fixed public ESPN scoreboard-page fallback is requested automatically only when its JSON endpoint returns 404 or a successful unsupported response format. It is **not attempted after 403/429/network denial** and is not a challenge bypass. It extracts embedded data from that one public page, not protected bookmaker pages. The same bounded parser is available for locally saved HTML.

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
- Timeouts, 16 MB source-response cap, 4 MB imports, 24 network requests per refresh maximum and at least 250 ms between source requests.
- Cache avoids refetching season stats/rosters every score refresh.
- Failed/blocked/rate-limited sources are surfaced in **Model lab → Data sources**. The code does not retry through bypasses.
- Desktop public-data requests originate on your computer, not in browser JavaScript, avoiding provider CORS requirements.
- Model price/stat freshness is stricter than cache availability. A cached history response does not make an old price fresh.

The reference/source software licenses do not grant NBA, ESPN, Basketball Reference or sportsbook data redistribution rights. Local caches are personal research artifacts, not committed datasets. Clear/delete your own cache if necessary; preserve any source attribution required by the provider.

## Sandbox limitation

The development environment allows outbound access only to GitHub/npm/PyPI hosts, so NBA/ESPN providers cannot be live-verified here. The default preview is **clearly synthetic**. Public endpoint schemas were translated from inspected GitHub reference definitions and fixture-tested. Test actual feed availability on Windows before assuming any quote/history coverage.
