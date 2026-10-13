# Input formats

Use the UI's **Import odds** button. All data is local. Demo imports are temporary; Live-data imports persist. JSON batches are validated completely before prices/features become visible. A quote's game/team must exist in the current schedule or in the same JSON batch.

The bundled sample generator creates **declared synthetic** data and must be used in **Demo**:

```sh
python scripts/make_sample_import.py > sample-import.json
```

Declared synthetic samples are rejected in Live mode. Real research inputs should not contain `"synthetic": true`. Do not relabel synthetic inputs as real.

## CSV quotes

Required columns (case-sensitive): `gameId,market,team,side,americanOdds,book,observedAt`. Add `player` for player markets and `line` for O/U. Optional `settlement` must be `full_game_including_ot`.

```csv
gameId,market,player,team,side,line,americanOdds,book,observedAt,settlement
REPLACE_GAME_ID,points,Stephen Curry,GSW,over,26.5,-110,Your book,2026-10-12T23:00:00Z,full_game_including_ot
```

**Replace the event ID and observation time** with the displayed real game/current price timestamp. This illustrative row is not a real quote. The app's CSV template generates an actual current-workspace game ID and UTC time but leaves American odds blank, so an untouched template cannot manufacture a price. For moneyline, leave player/line empty and use `home` or `away` with the corresponding team.

Quoted commas, escaped double quotes, CRLF/LF and multiline quoted fields are supported. A UTF-8 BOM is tolerated. Numbers must be complete numeric values, not currency strings or malformed text. Duplicate headers/column-count mismatches are rejected.

## Markets and settlement

| `market` | `side` | `player` | `line` |
|---|---|---|---|
| `moneyline` | `home` / `away` | Empty | Null / empty |
| `points` | `over` / `under` | Required | Integer or half point |
| `rebounds` | `over` / `under` | Required | Integer or half point |
| `assists` | `over` / `under` | Required | Integer or half point |
| `threes` | `over` / `under` | Required | Integer or half point |
| `pra` | `over` / `under` | Required | Integer or half point |
| `double_double` | `yes` / `no` | Required | Null / empty |
| `triple_double` | `yes` / `no` | Required | Null / empty |
| `first_basket` | `yes` / `no` | Required | Null / empty |
| `team_total` | `over` / `under` | Empty | Integer or half point |

Full-game including overtime only; no quarters, halves, alternate settlement scopes, void modeling or auto-settlement. DD/TD use all five official countable categories. First basket means **first made field goal**, not first point/free throw. Check player participation/DNP and first-basket rules at the sportsbook; the model does not implement book-specific void/refund policies.

American odds are integers `<= -100` or `>= +100`, bounded to ±100,000. Fractional/decimal odds, zero, `-99` and other ambiguous values are rejected. Prop lines are 0.5–150; team total lines 0.5–300. `observedAt` must be exact **UTC** `YYYY-MM-DDTHH:MM:SSZ`, not local time or a numeric epoch. Future prices more than 30 seconds ahead are rejected. Old prices import as stale and cannot claim current EV.

Use standard NBA abbreviations (`GSW`, `NYK`, `NOP`, `SAS`, `UTA`, `WAS`, etc.). IDs start with an ASCII letter/digit and contain only letters/digits, dots, colons, dashes and underscores. Prices are deduplicated by event/market/normalized player/team/side/line/book; a newer import for the same identity replaces it, including an explicit public feed quote.

## JSON quote document

```json
{
  "quotes": [
    {
      "gameId": "REPLACE_GAME_ID",
      "market": "points",
      "player": "Stephen Curry",
      "team": "GSW",
      "side": "over",
      "line": 26.5,
      "americanOdds": -110,
      "book": "Your book",
      "observedAt": "2026-10-12T23:00:00Z",
      "status": "open",
      "settlement": "full_game_including_ot"
    }
  ]
}
```

This is a format example, not a price recommendation. `status` may be `open`, `suspended` or `closed`. All imported records are forcibly marked user-supplied; an input cannot spoof provider provenance by setting `source`.

## Supplying feature history

The top-level document can additionally contain `games` (array), `teams` (array/object) and `players` (array/object). These records override matching public records and are never independently verified. Required numeric fields cannot be missing or silently become zero.

### Games

```json
{
  "id": "custom-research-1",
  "home": "LAL",
  "away": "GSW",
  "homeName": "Los Angeles Lakers",
  "awayName": "Golden State Warriors",
  "startsAt": "2026-11-05T01:00:00Z",
  "status": "upcoming",
  "seasonType": "regular"
}
```

This is a fictional illustrative matchup. Maximum 100 imported games. `status`: `upcoming`, `live`, `final`, `postponed`, `cancelled`. Live games also require `homeScore`, `awayScore`, `period`, `elapsedMinutes`, and a fresh UTC `dataAsOf`. The client is not permitted to call a scheduled game “live” just because its start time passed.

### Teams

Each team requires:

- `abbr` (when array-based; an object key can provide it).
- `pace` 70–130 possessions; `offRating`, `defRating` 70–150 points/100 possessions.
- `logs`: at most 150 records, each `{ "date": "YYYY-MM-DD", "points": 112, "margin": 7 }`; dates must be unique/real. Require at least 12 pre-game games for team forecasts. `home` is optional context.
- Optional `name`, `elo` (800–2200), `restDays` (0–30).
- Optional `dataAsOf` is retained and validated, not made fresh on restart. If absent, import-receipt time is recorded. Team environments >48 hours old cannot power live-data forecasts.

### Players

Each player requires `name`, `team`, `logs` (at most 150 unique game dates). Each log requires:

```json
{
  "date": "2026-10-01",
  "minutes": 34.5,
  "points": 27,
  "rebounds": 4,
  "assists": 6,
  "threes": 5,
  "steals": 1,
  "blocks": 0,
  "home": true
}
```

This is a hypothetical record. Optional context: `fgm`, `fga`, `ftm`, `fta`, `threeAttempts`, `oreb`, `turnovers`, `fouls`. Shooting fields are shown as context, not silently given fitted coefficients. `steals`/`blocks` are mandatory to evaluate DD/TD. Only games with at least 5 minutes and before start enter the regular-season player distribution; at least 12 valid games are required.

Optional player fields:

- `injury`: e.g. `Out`, `Questionable`, `Available`, `Unknown`. Reported-out/inactive blocks estimates; uncertain availability is warned.
- `projectedMinutes`: explicit 0.1–48 override, surfaced in model limitations.
- `historicalPace`: 70–130, the baseline pace of the supplied sample. Without it, current team pace is an explicit approximation.
- `dataAsOf`: valid UTC feature timestamp, preserved if provided.
- `live`: `{ "gameId": "...", "minutes": 21.5, "points": 19, "rebounds": 3, "assists": 5, "threes": 4, "steals": 1, "blocks": 0, "dataAsOf": "...Z" }`. Fresh values and the exact matching game ID are required for in-play props.
- `starterConfirmed`: boolean. If true, `starterGameId` is required and must match the target event; an old lineup does not confirm a new game. For first basket, every one of the 10 starters also needs `firstBasketGames >= 20` and `firstBasketWins` between 0 and games. Supply same-definition historical first-FG wins, not first points. First-basket-only players may have an empty `logs` array but still need complete first-FG history/lineup fields.

In-play player overtime rotations are not modeled; those forecasts are suppressed. A zero remaining horizon awaits a confirmed next period/final state. Already-achieved full-game thresholds are not offered as new edges.

## Saved HTML

Choose HTML in **File / paste**:

- Normalized `<script type="application/json">` input with the JSON structure above.
- Saved ESPN event-page embedded data (`__espnfitt__` or JSON scripts) for events/explicit prices. **Provide the original UTC price observation time** in the HTML form; old saved pages are not stamped fresh on import.
- Basketball Reference-style game-log tables with `data-stat` fields. Supply the player name/current NBA team. `mp="34:30"` becomes 34.5 minutes. DNP/incomplete rows are omitted.

The app reads local data only. No uploaded HTML/JS is executed. It will not follow URLs in a document or extract arbitrary bookmaker tables. Obtain data only when source terms permit it.

## Limits / persistence

Maximum 4 MB per import and combined saved-import document, 2,000 quote rows, 600 player/team records and 150 logs per record. Oversized/invalid batches fail before state changes. Persisted odds keep their original observation times. Quotes cannot be matched across different books/lines to manufacture a no-vig comparison.

Clear saved imports from Settings to remove user overrides; public cache is retained. Dataset source/provenance, available inputs, gaps, price age and caveats are shown in the research/model-lab views.
