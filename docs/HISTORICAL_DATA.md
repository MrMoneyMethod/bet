# Historical NBA play-by-play

## What is actually installed

**No Basketball Reference historical dataset is bundled, and no full historical crawl was performed in the Arena sandbox.** Direct sandbox HTTPS access to Basketball Reference is restricted. The parser and end-to-end integration are tested with explicitly synthetic HTML fixtures, not current provider responses. Installing a collector is not the same as collecting or validating all historical data.

The optional `scripts/basketball_reference_history.py` collector uses Python 3.10+ and its standard library only. The C++/Windows app still runs without Python or SQLite installed: it reads a compact, streamed `features.jsonl` export. All raw pages/events stay in the collector's local SQLite archive, outside Git and release packages.

## Permission and access rules

Read [Sports Reference data use](https://www.sports-reference.com/data_use.html), [terms of use](https://www.sports-reference.com/termsofuse.html), [bot traffic policy](https://www.sports-reference.com/bot-traffic.html) and [Basketball Reference robots.txt](https://www.basketball-reference.com/robots.txt). The data-use page restricts tools/databases built on scraped content and predictive/AI uses without permission. **Obtain permission appropriate to your use before bulk collection or model use.** `--permission-confirmed` is your confirmation, not permission granted by this application. Public access and a software license do not grant provider-data rights.

The collector:

- Identifies itself as `EdgecourtHistory`, never as a browser or a different crawler.
- Reads robots.txt on every run and fails closed if access rules cannot be verified.
- Uses a single worker, with **at least 3.1 seconds between requests**, including robots; a longer robots crawl delay increases that interval. This stays below the published 20-requests/minute limit. Do not run additional collectors from the same IP.
- Stops on network failures, challenges, redirects, 403, 429 and other non-404 errors. No proxies, challenge solving, login, access-denial fallback or automatic denial retries.
- Accepts only fixed HTTPS schedule, box-score and play-by-play paths on `www.basketball-reference.com`. It does not follow arbitrary links or execute page scripts.
- Limits a source response to 16 MB, checks cached page SHA-256 values and uses a single-writer lock.

## Authorized collection

Season arguments are **ending years**: `1997` means 1996–97. That is the default play-by-play-era starting point; this does **not** claim that play-by-play exists for every NBA game or for earlier seasons. NBA only, not ABA, WNBA, college or preseason. The default ending year follows the current NBA season, including an ongoing season's available completed games.

For a small authorized verification run first:

```sh
python scripts/basketball_reference_history.py crawl \
  --permission-confirmed --start-season 2026 --end-season 2026 \
  --max-games 5 --output edgecourt-data/history
```

Inspect the archive, schema compatibility and coverage before a full run. From a network where access is permitted:

```sh
python scripts/basketball_reference_history.py crawl \
  --permission-confirmed --start-season 1997 --end-season 2027 \
  --output edgecourt-data/history
```

On Windows use one line instead of shell backslashes. A full multi-decade crawl can require **many tens of thousands of requests and several days**, plus substantial disk space. Run this as a deliberate local research job, not a browser refresh. Adjust the ending year to the season you actually need.

The same command resumes: completed game IDs and cached pages are not downloaded again. `Ctrl+C` preserves completed games and emits a partial feature bundle. `--max-games N` is a per-run limit; it does not relabel a small sample as full coverage. `--refresh-schedules` explicitly re-discovers schedules for ongoing seasons. `--retry-missing` retries previously missing/invalid pages on a later permitted run; it never bypasses an access denial. If a process crashes and leaves `collector.lock`, verify it has stopped before manually removing the lock.

```sh
python scripts/basketball_reference_history.py status --output edgecourt-data/history
python scripts/basketball_reference_history.py export \
  --as-of 2026-10-13 --output edgecourt-data/history
```

`--as-of` is an **exclusive date cutoff** for model features, not a claim of historical betting-price availability. All games on or after that date are excluded from exported features. Defaults to today's UTC date. `--recent-days` defaults to 210 and chooses recently active players; it does not remove older raw games/events from the archive.

## Authorized saved-page ingestion (no network)

You can ingest pages you are permitted to use, without provider requests. Preserve this directory structure:

```text
saved-pages/
  leagues/NBA_2026_games-april.html        # optional saved schedules
  boxscores/202604010BOS.html             # full-game basic box scores for both teams
  boxscores/pbp/202604010BOS.html          # matching complete play-by-play
```

The ID above is a **filename-format example**, not a bundled real game. Use the actual matching source game ID. When schedules are absent, explicitly provide the season ending year, especially for unusual calendar seasons such as the 2020 bubble:

```sh
python scripts/basketball_reference_history.py ingest \
  --permission-confirmed --season 2026 --saved-html saved-pages \
  --output edgecourt-data/history
```

Saved-page discovery is always labeled partial/unverified, even when every supplied file parses. It cannot prove the directory contains every game in a season.

## Connect it to Edgecourt

Use the same local data directory, or point at the feature bundle separately:

```sh
./build/Edgecourt --live --data-dir edgecourt-data
# Windows:
Edgecourt.exe --live --data-dir edgecourt-data
# Alternative bundle location:
Edgecourt.exe --live --history-dir C:\YourFolder\history
# Local history/imports only, with no ESPN/NBA network requests:
Edgecourt.exe --live --offline --data-dir edgecourt-data
```

Without `--history-dir`, the app reads `<app-data-dir>/history/features.jsonl`. The desktop default app-data directory is `%LOCALAPPDATA%\Edgecourt` on Windows and `$XDG_DATA_HOME/edgecourt` or `~/.local/share/edgecourt` on Linux. The collector's default is repository-relative `edgecourt-data/history`; use the explicit paths above to align them.

After collecting or re-exporting, use **Model lab → Historical play-by-play → Reload local history**, or restart the app. Reload is token/origin guarded and reads only the configured local bundle, not a browser-supplied file path or URL. The entire replacement validates before being accepted. A failed reload retains the previous bundle. Removing the file and reloading removes its features rather than silently retaining old history. Demo never applies historical features.

The coverage panel reports archived game/event counts, date range, recently exported players and missing/rejected game counts. “All discovered completed games” is scoped to requested, successfully discovered schedules; it is **not independently verified all-history coverage**. Season discovery failures, pending games, 404s and rejected games remain explicit gaps. You can inspect per-season coverage and missing examples with `status`.

## What the model uses

1. **Whole historical player vectors.** Paired full-game box scores supply minutes, P/R/A, threes, steals, blocks and shooting/turnover/foul context. Up to 150 recent regular-season records are exported; the existing model selects at most 60 eligible pre-game observations. Playoff vectors are not mixed into the regular-season distribution. First-FG observations may include regular-season and playoff starts. All raw events remain archived, but the app does not indiscriminately fit current props on retired-player or decades-old game samples.
2. **Dated first-made-field-goal opportunities.** The first recognized made 2/3-point field goal from a verified opening is the winner. An earlier free throw is not a first basket. Exactly five historical starters from each paired box score receive an opportunity; DNP/bench participation is not treated as a historical start. Unexplained opening score changes disable that game's first-FG feature rather than guess a scorer. Up to 2,000 dated observations per active player are exported, and the engine excludes the target day and future dates at evaluation time.
3. **Team environment fallback.** Recent paired regular-season box totals supply scoring/margin histories and a documented approximate possessions baseline: average of each team's `FGA − ORB + TOV + 0.44 × FTA`, normalized to 48 minutes for pace. Offensive/defensive ratings use points per estimated possession. These are approximations, not official possession counts or fitted coefficients. Fresh complete NBA ratings, roster/injury status and overlapping official game logs take precedence. Explicit user feature imports override archive fallback features.
4. **Current lineups remain separate.** A past starter is never confirmed for a new game. Supply game-scoped `lineups` in a JSON import (see [IMPORTS.md](IMPORTS.md)). All ten current starters, five per team, still need at least 20 eligible first-FG observations. Reported-out players and incomplete fields block the first-basket market.

At least 12 regular-season player/team games and the existing price/stat freshness, roster, minutes, settlement and preseason guards still apply. `dataAsOf` stays anchored to historical game dates; collecting/exporting/reloading old games does not make an old team environment fresh. Historical source flags and sampling limitations are shown in the research panel.

**Not implemented or claimed:** trained/calibrated machine learning, live possession-level forecasts, reconstruction of executable historical sportsbook prices, first-point settlement, automatic current lineup inference or proven accuracy/profit. More archived events do not by themselves validate an edge. The current empirical-bootstrap/frequency model remains experimental.

## Archive and bundle format

- `history.sqlite3`: metadata, requested/discovered seasons, resumable game statuses, normalized per-game records and **every parsed timed event**, keyed by game ID and source-order sequence. Period boundaries, tied-clock rows, overtime, descriptions, linked player IDs, teams and cumulative scores are retained.
- `pages/*.html.gz`: original bounded saved/downloaded HTML, keyed by content hash, with source URLs/retrieval times recorded in SQLite.
- `features.jsonl`: atomically written, schema-versioned manifest followed by one team/player feature record per line. Runtime limits: 128 MB total, 1 MB per line, 1,200 recent players, 30 teams; each record uses the normal C++ feature validator. Source claims are still local/user-supplied, not independent verification.

Raw tables must contain both complete basic box scores, explicit historical starter sections, required nonmissing stats, all period starts/ends and a final PBP score matching the box score. Invalid games are tracked rather than replaced with fabricated data. Provider schema changes require inspection and updated fixtures. Source completeness cannot be established solely from final-score agreement.

Keep archives in the ignored `edgecourt-data/` directory, or external storage. Do not commit or distribute raw provider datasets or put them in releases. The fixture generators under `tests/` are synthetic software tests, not historical research inputs.

## Keyless alternative: SportsDataverse / hoopR CSVs

The hoopR project publishes the same class of historical data (ESPN-sourced
play-by-play 2002-present and player box scores) as open GitHub release datasets.
Download the per-season CSVs on your own machine, for example:

```bash
gh release download espn_nba_pbp --repo sportsdataverse/sportsdataverse-data --pattern 'play_by_play_2024.csv'
gh release download espn_nba_player_boxscores --repo sportsdataverse/sportsdataverse-data --pattern 'player_box_2024.csv'
python3 scripts/basketball_reference_history.py hoopr --permission-confirmed \
  --season 2024 --player-box-csv player_box_2024.csv --pbp-csv play_by_play_2024.csv \
  --output edgecourt-data/history --as-of 2024-06-01
```

The importer matches games by CSV game id, requires exactly five starters per team
and home/away columns, and verifies the first field goal against the archived
play-by-play. Unknown or renamed hoopR headers fail loudly with the missing-column
list. Player ids are namespaced `espn-...` so the datasets never collide; the
app-side name crosswalk is unchanged. (GitHub release assets could not be downloaded
inside the development sandbox, so the importer is tested against fixtures written
to the hoopR schema; run it once locally to validate the current season's headers.)
