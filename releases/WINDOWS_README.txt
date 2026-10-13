EDGECOURT 0.3.0 - WINDOWS 10/11 x64
==================================

1. Extract this ZIP to a folder you control.
2. Double-click Edgecourt.exe.
3. Your default browser opens the local dashboard.
4. Keep the executable's console window open; Ctrl+C/closing it stops the app.

No API keys, Python, Node, OpenSSL install, account, or external web server
is required to run it. HTML/CSS/JS are embedded in the executable.
The default server listens only on 127.0.0.1, preferably port 8741.
If automatic browser launch fails, copy the printed local URL into a browser.

DEMO VS PUBLIC DATA
-------------------
Demo is fully synthetic: fictional matchups, histories and prices.
Live data requests public ESPN/NBA sources. If they fail, there is no silent
fallback to demo games or invented real prices. Check Model lab / Data sources.

Public moneylines are shown ONLY when explicitly present in the source.
There is NO bundled reliable free/keyless all-books player-prop odds feed.
Use Import odds to enter a current quote or import CSV/JSON prices/features.
The CSV template uses a current-workspace game ID, but leaves odds blank.
Supply real prices; an untouched template cannot manufacture a quote.
All odds are shown/entered as implied percentages, including book margin,
separately from Model %. Legacy American odds in files remain supported.

Supported markets: moneyline, points, rebounds, assists, threes, PRA,
double-double, triple-double, first basket, team totals O/U.
No quarter/half markets. Full game including overtime. First basket means
first made FIELD GOAL; verify identical settlement at the book.

Missing histories, stale live boxes, out players and preseason rotations
can disable projections. First basket requires all 10 confirmed starters and
historical first-FG counts; confirmations must match starterGameId.
These are not guessed from points per game. Live player overtime rotations
are not modeled; those forecasts are suppressed. Achieved full-game
thresholds are not treated as new edges.

HISTORICAL PLAY-BY-PLAY (OPTIONAL)
---------------------------------
No full Basketball Reference crawl was run in the sandbox and no dataset
is bundled. The optional scripts/basketball_reference_history.py collector
requires Python 3.10+ separately; the executable still runs without Python
or SQLite installed. Obtain appropriate Sports Reference permission before
collecting or using its data. The collector is permission gated, obeys robots,
waits at least 3.1 seconds between requests, resumes from a local SQLite
archive, and stops on blocks/rate limits without bypasses.

See docs/HISTORICAL_DATA.md. The app loads history/features.jsonl under its
data directory, or use --history-dir PATH. Model lab shows archive counts,
gaps, date range and Reload local history. Current quotes/schedule and all
ten actual current starters are still needed. Historical starters never
confirm a new matchup. Dated first-FG history excludes the target day/future
observations. Raw all-season events stay outside the executable/Git.

IMPORTANT LIMITATIONS
---------------------
The model is an EXPERIMENTAL UNCALIBRATED BASELINE, not a trained/proven
profitable model. "Model +EV" is only an estimate under its assumptions.
No measured win rate or profit claim is made. Synthetic results do not
validate a model. There is no auto-betting or sportsbook login.

Keyless providers may block requests, change schemas or stop offering data.
Live provider responses were NOT verified from the restricted sandbox.
The Windows binary was cross-compiled and inspected, NOT runtime-tested
on Windows here. Native Linux core/API/browser integration was tested.

This executable is unsigned. SmartScreen may warn. Inspect the source or
build it yourself and verify SHA256SUMS.txt. Do not disable OS protections.

DATA / PRIVACY
--------------
Settings, imports and cache: %LOCALAPPDATA%\Edgecourt
Watchlist/bankroll display preferences: this browser's localStorage.
Imported price timestamps are preserved, not refreshed on restart.
EV expires after 15 minutes pregame and 90 seconds live.
Demo imports are temporary and reset on manual demo refresh.
Use only legally/age-appropriately and in accordance with provider terms.
No guarantee of profit. Never stake funds you cannot afford to lose.

CLI OPTIONS
-----------
Edgecourt.exe --help
Edgecourt.exe --demo
Edgecourt.exe --live
Edgecourt.exe --port 8741 --no-browser
Edgecourt.exe --data-dir "C:\YourFolder\EdgecourtData"
Edgecourt.exe --backtest samples\backtest-example.json
Edgecourt.exe --live --history-dir "C:\YourFolder\history"
Edgecourt.exe --live --offline --data-dir "C:\YourFolder\EdgecourtData"

The bundled backtest example is FICTIONAL, not real model performance.
See docs\IMPORTS.md, docs\MODEL.md and docs\DATA_SOURCES.md for details.
Source code is in the MrMoneyMethod/bet repository.
