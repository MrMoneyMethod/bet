# Architecture and security

```text
Edgecourt.exe
  ├─ embedded HTML / CSS / JS  → default browser
  ├─ cpp-httplib local HTTP server
  │   ├─ GET  /api/health, /api/state, /api/template.csv
  │   └─ POST /api/mode, /api/refresh, /api/import, /api/clear-imports, /api/history/reload
  ├─ C++ eligibility + prediction engine
  ├─ one bounded public-data refresh worker
  ├─ WinHTTP on Windows / exec'd curl on Linux (no shell)
  └─ local cache, settings.json and imports.json
```

## Files

- `src/core.*`: mathematics, feature inventory, synthetic fixtures, import validators, EV/no-vig aggregation, request-time eligibility expiry and backtest scoring.
- `src/feeds.*`: OS HTTPS transport, source cache/request budgets, ESPN/NBA/exchange odds parsers, optional API-Sports, SharpAPI and Sportsbook API2 moneyline adapters with environment-only credentials and source-specific rate controls, plus NBA_Go, TheSportsDB V2 and SportScore context-only adapters.
- `src/history.*`: bounded JSONL archive-feature loading/validation, current/public priority merge and Demo isolation; raw all-season events are not loaded into the native app.
- `scripts/basketball_reference_history.py`: optional, authorized standard-library collector with a resumable SQLite archive, raw gzip cache and atomic runtime feature export.
- `src/app.*`: HTTP routes, session token, persistence, mutex-protected workspace state and refresh worker.
- `src/main.cpp`: executable CLI, browser launcher, loopback defaults, shutdown handling.
- `web/*`: responsive browser UI; no CDN fonts, third-party scripts, images or external API calls.
- `scripts/embed_assets.py`: build-time asset embedding. Assets are part of the executable, not runtime files that must be found beside it.
- `tests/*`: core/feed fixture, native API and optional browser integration tests.

## Workspace state

Default first-run mode is Demo. A saved user choice may open Live on later runs. Demo is seeded synthetic research data; provider failures never cause a mode fallback. User imports made in Demo are temporary and never persist into real-data mode.

Live refresh runs on one worker and commits a completed snapshot under a mutex. A generation counter discards stale in-flight results after a mode switch/reset. Imported records are re-applied at commit time so an in-flight feed refresh cannot overwrite a newly accepted user import.

Imports validate the complete document, resolve quoted game/team identities and write persistence **before** changing visible state. Invalid batches are rejected without partial merging. Quotes are deduplicated by event/market/player/team/side/line/book. Imported feature records explicitly override their public counterpart and are marked user-supplied.

Cached model computations are not allowed to freeze eligibility. `/api/state` rechecks price age, live-data age and passed scheduled starts at request time. The browser polls every 15 seconds while visible. Scores/boxes/odds require a new provider/import retrieval to refresh their real-data timestamp.

## Storage

- Windows: `%LOCALAPPDATA%\Edgecourt`.
- Linux: `$XDG_DATA_HOME/edgecourt`, otherwise `~/.local/share/edgecourt`.
- `--data-dir` overrides this for tests/development.
- `history/features.jsonl`: optional runtime feature bundle, with honest archive counts/coverage. `--history-dir` overrides its local directory. Raw `history.sqlite3` and `pages/*.html.gz` are collector artifacts, not served or shipped by the app.
- `settings.json`: selected mode.
- `imports.json`: validated real-data workspace imports; quote times preserved.
- `cache/*.json`: bounded provider response caches keyed by URL hash, with original retrieval times and URLs. `api-basketball-usage.json` and `sportsbook-api2-usage.json` store only UTC-day counters and quota metadata—never keys, account names or email fields.

Writes use a temporary file then replacement (`MoveFileExW` on Windows, filesystem rename on Linux). No external database or model download is needed to run the native app. Optional historical collection uses Python's built-in SQLite; the executable reads only its streamed feature export. Watchlist/bankroll/display settings (including the light/dark/system appearance preference) use this browser's localStorage; they are not cloud-synced and are not encrypted.

## Network boundaries

The desktop default binds **127.0.0.1**, not all network interfaces. Each process gets a random write token returned by the same-origin state API. Mutations require the token and a matching Origin when supplied. Host validation rejects unrelated names/DNS-rebinding targets; loopback hosts and Arena `.e2b.app` previews are accepted. CORS is not enabled.

The preview deliberately binds `0.0.0.0` and permits iframe embedding. The **GET state endpoint is not an authenticated multi-user service**. Do not publicly expose real personal imports or treat `--host 0.0.0.0` as a production deployment. Browser backend calls are relative, never browser-facing localhost requests to a separate service.

`--offline` disables public refresh requests without switching real imports/history to synthetic Demo. Historical reload validates a full replacement before swapping it in; old archive features are rebuilt from the last public baseline plus explicit imports, so a removed/replaced bundle cannot leave stale historical vectors behind.

Source requests use fixed HTTPS allowlisted hosts and cannot follow redirects or request arbitrary URLs. Optional keys `API_BASKETBALL_KEY`, `SHARPAPI_KEY`, `RAPIDAPI_KEY`, `THESPORTSDB_KEY` and `SPORTSCORE_KEY` are read from the process environment and sent only as provider-specific headers to exact hosts; NBA_Go receives no key. Sportsbook API2 additionally stays idle until non-secret `RAPIDAPI_HOST` exactly matches its configured host, a confirmation step because the RapidAPI product host is not live-verified here. Keys are never compiled, sent to browser code, logged, or persisted. API-Sports and Sportsbook API2 quota records store daily counters only. Linux curl is spawned with an argument vector, not a shell command; sensitive headers are read from a private socket pipe, never placed in curl arguments, and API-key environment variables are removed from curl's child environment. Windows browser launch receives only a generated loopback URL. There are no sportsbook login credentials, betting actions, uploads to third-party services or telemetry.

## Content safety

- No JS execution in the HTML extractor.
- Imported/provider text is escaped before UI HTML interpolation.
- CSS colors and SVG icons come from fixed local tables, not imported HTML.
- CSP restricts scripts, connections, images and fonts to the local app; inline styles are allowed for charts and team colors. No inline script is required.
- No open file-serving directory, file-path import API or arbitrary source URL fetch endpoint.
- Structured import size/count/value constraints and UTC time validation.
- Exported CSV text cells with formula-leading characters are prefixed with an apostrophe; numeric data remains numeric.

This is defense-in-depth for a personal local app, not a formal security audit. Protect local files and your computer as usual.
