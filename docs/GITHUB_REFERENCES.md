# GitHub reference scan

Reviewed on **2026-10-12 (America/Denver)** using GitHub search/API. **33 repositories screened**; endpoint definitions, 13 relevant READMEs and 5 additional sportsbook-interface sources were read, with remaining candidates screened at repository metadata/search-summary level. This is a documented broad scan, not a claim to have audited every project, validated their accuracy claims or read every line.

## Selection criteria

- An explicit reusable license; public visibility alone is not a reuse license.
- Fit for a single native Windows executable, low runtime/dependency burden and well-maintained foundational libraries.
- Transparent source/provenance and actual price-versus-estimate separation.
- Chronological leakage guards, covariance-aware props and honest calibration/validation limits.
- Free/keyless compatibility, not hidden dependence on a paid Odds API or obsolete sportsbook route.
- Current data access is not inferred from stars, old screenshots or claimed win rates.

**Adopted dependencies:** cpp-httplib and nlohmann/json. **Primary schema reference:** nba_api. Other projects informed research or were rejected, not bundled wholesale. New model/adapters are original C++ implementation; no unlicensed model code or trained artifacts were copied.

## Screened projects

| Repository | Detected license | Stars at scan | Decision / scope |
|---|---|---:|---|
| [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | MIT | 16,910 | **Adopted**: small vendored C++17 local HTTP server; pinned v0.22.0. |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | 50,755 | **Adopted**: robust schema parsing/state/persistence; pinned v3.12.0. |
| [swar/nba_api](https://github.com/swar/nba_api) | MIT | 3,801 | **Primary endpoint reference**: inspected player/team logs, advanced-team stats, live scoreboard/boxscore schemas and HTTP behavior; standings (`LeagueStandingsV3`), player season stats (`LeagueDashPlayerStats`) and team Four Factors (`LeagueDashTeamStats`) adapters were later re-derived from the same endpoint definitions at the pinned commit. Original C++ adapters, not a Python runtime dependency. |
| [bttmly/nba](https://github.com/bttmly/nba) | MIT | 734 | README reviewed: NBA endpoint design, IP blocking and browser CORS caveats. Prefer native OS HTTPS. |
| [jaebradley/basketball_reference_web_scraper](https://github.com/jaebradley/basketball_reference_web_scraper) | MIT | 560 | README reviewed: historical game-log field conventions; limited saved-HTML data-stat extraction rather than bulk network crawling. |
| [parlayparlor/nba-prop-prediction-model](https://github.com/parlayparlor/nba-prop-prediction-model) | MIT | 32 | README reviewed: raw game-log/rolling-average prop baseline and data export. Expanded into joint C++ bootstrap with minutes/pace/eligibility guards. |
| [flancast90/sportsbookreview-scraper](https://github.com/flancast90/sportsbookreview-scraper) | MIT | 54 | README/tree reviewed: historical odds/data separation. Old 2011–2021 archives are not a current-live prop feed; code/data not bundled. |
| [freshened/nba-prop-analytics](https://github.com/freshened/nba-prop-analytics) | MIT | 3 | README reviewed: joint/Monte Carlo prop-analysis concepts. Paid/keyed Odds API path not adopted. |
| [RaudyG/nba-game-prediction](https://github.com/RaudyG/nba-game-prediction) | MIT | 0 | README reviewed: leakage-safe feature engineering, chronological splits and baseline-aware evaluation concepts. No trained model copied. |
| [Pirkn/NBA-Game-Outcome-Prediction](https://github.com/Pirkn/NBA-Game-Outcome-Prediction) | MIT | 12 | README reviewed: Elo/team-rating model concepts; complex Python ensemble not a portable C++ runtime dependency. |
| [muyangye/NBA_Prediction](https://github.com/muyangye/NBA_Prediction) | MIT | 4 | README reviewed: Elo, offense/defense and team-stat feature families. Implemented original transparent C++ heuristics, not copied weights. |
| [Airkid29/TruthMarket](https://github.com/Airkid29/TruthMarket) | MIT | 1 | README reviewed: probability-versus-market-price separation. No real Polymarket trading/execution integration adopted. |
| [cwendt94/espn-api](https://github.com/cwendt94/espn-api) | MIT | 984 | README reviewed: public ESPN ecosystem and data context. Fantasy API is not a live sportsbook price provider. |
| [cmunch1/nba-prediction](https://github.com/cmunch1/nba-prediction) | No license detected | 311 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [kyleskom/NBA-Machine-Learning-Sports-Betting](https://github.com/kyleskom/NBA-Machine-Learning-Sports-Betting) | No license detected | 1,742 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [whisdev/NBA-prediction-sports-betting](https://github.com/whisdev/NBA-prediction-sports-betting) | No license detected | 90 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [nealmick/Sports-Betting-ML-Tools-NBA](https://github.com/nealmick/Sports-Betting-ML-Tools-NBA) | No license detected | 88 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [luke-lite/NBA-Prediction-Modeling](https://github.com/luke-lite/NBA-Prediction-Modeling) | No license detected | 53 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [lazarobeas/NBAProject](https://github.com/lazarobeas/NBAProject) | No license detected | 10 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [nihalafs11/StatsPicksNBA](https://github.com/nihalafs11/StatsPicksNBA) | No license detected | 7 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [akhilpenumudy/NBA-Player-Prop-Cheat-Sheet-Maker](https://github.com/akhilpenumudy/NBA-Player-Prop-Cheat-Sheet-Maker) | No license detected | 5 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [nlefrancois6/NBA_prediction](https://github.com/nlefrancois6/NBA_prediction) | No license detected | 2 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [LuuVi2911/NBA-Betting](https://github.com/LuuVi2911/NBA-Betting) | No license detected | 1 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [seanj2508/SportsBetPredictor](https://github.com/seanj2508/SportsBetPredictor) | No license detected | 0 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [ANickFlower/nba-props-app](https://github.com/ANickFlower/nba-props-app) | No license detected | 0 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [gboogy/nba-injury-data-scraper](https://github.com/gboogy/nba-injury-data-scraper) | No license detected | 2 | Metadata/search-summary screen only. **Not reused:** no explicit license detected; prediction results/claims not independently validated. |
| [boostorg/math](https://github.com/boostorg/math) | BSL-1.0 | 377 | Metadata screen: strong numerical library, but basic normal CDF uses std::erfc; avoid a heavy dependency for v1. |
| [lexbor/lexbor](https://github.com/lexbor/lexbor) | Apache-2.0 | 2,101 | Metadata screen: full HTML parsing library is unnecessary for the bounded saved-JSON/data-stat extractor; no renderer/JS execution needed. |
| [auroradan/PrizePicks-Prop-Finder](https://github.com/auroradan/PrizePicks-Prop-Finder) | No license detected | 18 | Endpoint/schema research only. No code adopted. Legacy public routes are not asserted to remain usable; no challenge-bypass implementation included. |
| [dsullo1216/nba-predictions](https://github.com/dsullo1216/nba-predictions) | No license detected | 0 | Endpoint/schema research only. No code adopted. Legacy public routes are not asserted to remain usable; no challenge-bypass implementation included. |
| [bslaski10/p6pickfinder](https://github.com/bslaski10/p6pickfinder) | No license detected | 1 | Endpoint/schema research only. No code adopted. Legacy public routes are not asserted to remain usable; no challenge-bypass implementation included. |
| [apsh0tDev/betseeker](https://github.com/apsh0tDev/betseeker) | No license detected | 1 | Endpoint-family review only. **Rejected** code/proxy/CAPTCHA bypass approaches; no unlicensed code or application access IDs included. |
| [jbl306/labs-wiki](https://github.com/jbl306/labs-wiki) | MIT | 1 | MIT-licensed April 2026 technical note reviewed: reports blocked legacy DraftKings routes and newer browser-discovered interfaces. **Not independently verified here**; supports not promising a reliable keyless prop feed. |

## Exact version/provenance records

Application headers are pinned independently of the scanned default branches:

- cpp-httplib **v0.22.0**, commit `b6c55c6030f5160d1a360a5a5180ba3205e2ce2f`.
- nlohmann/json **v3.12.0**, commit `55f93686c01528224f448c19128836e7df245f72`.
- NBA schema reference inspected at commit `52c0fb0b81c4b785d6cebeb9988799e5d796dfd7`.

`reference-scan.json` records the GitHub-reported license, default-branch commit and star snapshot for each candidate. License detection is not a complete legal audit; check upstream file-level terms before expanding reuse. Stars describe popularity, not model quality or predictive evidence.

## Rejected shortcuts

- Do not treat no-license public betting repos as free source/model assets.
- Do not turn synthetic demo odds into apparent real bookmaker prices.
- Do not claim a current free all-prop feed from an old scraper/archive. A reviewed April 2026 sportsbook-interface note reports legacy endpoints blocked; this is reference evidence, not a sandbox verification. Direct feed coverage is not promised based on stale route examples.
- Do not convert points-per-game into first-basket probability without first-FG history and lineups.
- Do not count 8,192 Monte Carlo draws as 8,192 independent historical games.
- Do not present an untrained heuristic as a profitable calibrated machine-learning model.

Research links are references, not endorsements or guarantees of data access. Source software licenses do not grant rights to redistribute NBA/ESPN/bookmaker data.
