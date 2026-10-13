# Prediction engine v1: experimental and uncalibrated

Implementation: `src/core.cpp`. Model identifier: `weighted-bootstrap-v1-experimental`.

This is a transparent research baseline, not trained ML. Coefficients are documented heuristics, not fitted or proven optimal. No out-of-sample profitability, calibration, accuracy, closing-line-value or win-rate claim is made. Synthetic demo results cannot validate anything.

## Player statistics

1. Select at most the 60 newest historical games with at least 5 minutes played, strictly before the target start. Require at least 12 games. DNPs/very-low-minute games are excluded: this can bias the sample toward active participation and must not be mistaken for a fully injury-conditioned distribution.
2. Weight historical game index `i` with `exp(-0.045 × i)`, multiplied by `1.15` for the same home/away location. These constants are fixed heuristics.
3. Estimate minutes as `0.7 × last-5 mean + 0.3 × weighted historical mean`, capped at 6–42. Explicit user overrides (0.1–48) are exposed.
4. Adjust the historical stat rate by `(team pace + opponent pace)/2 / historical team pace`, clipped to 0.85–1.15. If player-specific historical pace is not supplied, the current team pace is used as an **explicit approximation**; historical opponent pace for every sample game is not reconstructed. Off-season pace/role changes remain a limitation.
5. Draw 8,192 weighted historical **whole stat vectors**. Scale all dimensions by projected minutes / sampled minutes, pace ratio and a shared mean-one lognormal perturbation with log-SD `0.08`. Round each category to a nonnegative integer.
6. Compare the discrete sampled outcomes to the actual quoted line. Integer lines can push; half-point lines cannot. Over and under estimates use the same seed/distribution, so their mass plus push mass partitions one.

Whole-vector resampling preserves empirical covariance (with a shared perturbation); independent points/rebounds/assists assumptions are avoided. It does **not** establish that future correlations or tails will match historical data. No per-matchup opponent defensive effect is currently fitted for player props; that future input is marked unimplemented rather than secretly applying arbitrary coefficients.

PRA is the sum of the sampled P/R/A vector. Double-/triple-double events count at least 2/3 categories reaching 10 across **points, rebounds, assists, steals and blocks**. Missing STL/BLK blocks those markets, rather than treating missing categories as zeros.

### Live player props

Require a live game and fresh actual player minutes/stats (maximum 90 seconds). Remaining expected minutes are bounded by both the pregame opportunity estimate minus accrued minutes and game-time remaining × projected minutes / 48. Final stat samples are accrued actual values plus sampled remaining production. Overtime changes the team-market horizon, but player rotations and possession-level state are not modeled: live player props are suppressed during overtime. A zero remaining horizon waits for an explicit next period/final status, and already-achieved full-game thresholds are not treated as new edges. Expired forecasts clear sizing fields too.

This is an especially weak live baseline: substitutions, fouls, usage changes, blowouts, injury restrictions, possessions and book latency are not modeled. Missing/expired accrued data disables probability/EV instead of substituting pregame averages.

## Moneyline

Require offense, defense and pace for both teams and at least 12 historical team games.

- Matchup pace = average home/away pace.
- Baseline home points = pace × `(home offense + away defense)/200 + 1.15`.
- Baseline away points = pace × `(away offense + home defense)/200 − 1.15`.
- Without Elo: expected margin = baseline home − away points + `0.5 × clipped rest differential`.
- With Elo: margin = `0.7 × rating-derived margin + 0.3 × (Elo difference/28 + 2.3) + 0.5 × rest differential`.
- Rest differential is clipped to ±3 days. Home court is a fixed 2.3-point baseline.
- Use a normal margin distribution with empirical home-team margin SD clipped to 9–18 points. Win probability is `Phi(expected margin / SD)`; away probability is its complement.

Elo starts at 1500 within the fetched historical window, uses K=20 and a 65-rating home adjustment, and updates chronologically only after paired resolved games. This is **windowed**, not a complete multiseason trained rating, and does not model roster transitions or seasonal regression. Rating/Elo features partially overlap; the mixture is heuristic.

Live moneyline conditions expected margin on the actual current score difference and the remaining fraction of the game. SD scales with sqrt(remaining fraction), with a 1.5-point floor. Ties are treated through a continuous winner baseline; explicit overtime-resolution dynamics are not learned.

## Team totals

Use the above team mean with historical team point SD (9–18). Live totals add accrued actual score to expected remaining points. Integer/half-point settlement uses a normal distribution with discrete continuity boundaries:

- `P(under) = Phi((ceil(line) − 0.5 − mean)/SD)`
- `P(over) = 1 − Phi((floor(line) + 0.5 − mean)/SD)`
- On integer lines, `P(push) = 1 − P(over) − P(under)`.

This discretized normal baseline is not a calibrated team-scoring model.

Live player overtime rotations are not modeled: those forecasts are suppressed. A zero remaining horizon awaits an explicit next period/final status, and already-achieved full-game thresholds are not offered as new edges. Expired forecasts also clear sizing fields.

## First basket

No guessed probabilities from PPG or scoring rate. Require:

- A pregame, non-preseason event.
- Exactly 10 starters confirmed for the target event (`starterGameId`) across both teams, no contradictory reported-out starter.
- At least 20 historical first-made-field-goal observations for every starter.

Jeffreys-smoothed historical frequency `(wins + 0.5)/(games + 1)` is normalized across the complete field. “No” is the complement. Counts are explicit supplied features; the public NBA/ESPN adapters do **not** produce a complete historical first-basket corpus automatically. Frequency normalization is a matchup-independent approximation and does not account for tip-off possession, opening plays or lineup interactions.

Settlement is **first made field goal** (not first free throw/point). Only import matching prices/rules. Live first-basket markets are disabled once a game starts.

## EV, fair prices and sizing

For American odds `a`:

- If positive: decimal `d = 1 + a/100`.
- If negative: decimal `d = 1 + 100/abs(a)`.
- `EV percent = 100 × (P(win) × d + P(push) − 1)`.
- This is expected **profit** per $100 staked; displayed expected gross return is `$100 + EV`.

No-vig implied probability is `implied_side/(implied_side + implied_opposite)`. Only same-game/market/subject/line/book opposite selections, both fresh and at most 90 seconds apart, qualify. Model edge compares conditional-on-no-push win probability with that no-vig benchmark. No matching opposite quote means no fair-price/edge figure.

Optional quarter-Kelly uses `(b×P(win) − P(loss))/(b×(P(win)+P(loss))) × 0.25`, with `b=d−1`, clipped to 0–2% of the illustrative bankroll. It is **off by default** and not actionable advice. Uncalibrated inputs make Kelly particularly dangerous.

## Samples are not confidence

Effective historical N is `(sum weights)^2 / sum(weights^2)`. A Wilson 95% sampling interval uses that N, **not 8,192 simulated draws**. For teams/first basket, historical counts are used. This interval ignores model error, correlation across games, source errors, selection bias and nonstationarity. “Larger/moderate/small sample” is a sample-size label, not a confidence grade.

The lower-bound EV is a sampling sensitivity calculation, not proof that an edge is robust.

## Suppression and data guards

No EV for absent, suspended, expired or future-dated prices; no probability for final/postponed/cancelled games, unconfirmed live transition after start, unsupported settlement, preseason, missing history/pace, reported-out players or stale live stats. Official/imported team environments older than 48 hours are suppressed in live-data mode. Historical player samples older than 210 days are rejected; >45 days is explicitly warned.

All data timestamps are UTC. The browser formats starts in the viewer's timezone. Imported odds timestamps are retained across refresh/restart, not overwritten by retrieval time. ESPN moneylines often have no reliable book-last-update time: retrieval age is shown and a verification warning is mandatory.

## Backtesting CLI

`--backtest` scores **supplied predictions**, not automatically reconstructed historical feature sets, not training, and not proof of authentic timestamps. `predictedAt` must precede `startsAt`. Optional `pushProbability` affects EV selection. Brier/log loss score resolved outcomes conditional on no push; actual pushes refund flat unit stakes and are excluded from binary scores.

A credible next step requires a timestamped historical feature/obtainable-price corpus, walk-forward evaluation, held-out calibration, realistic settlement/limits/latency, all picks rather than cherry-picked winners, closing-line comparisons and adequate independent samples. Do that before using estimates financially.
