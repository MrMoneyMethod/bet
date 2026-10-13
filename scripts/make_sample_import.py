#!/usr/bin/env python3
"""Generate a declared SYNTHETIC feature/quote example. Import in Demo only."""
import json
import random
from datetime import datetime, timedelta, timezone

rng = random.Random(20261012)
now = datetime.now(timezone.utc).replace(microsecond=0)
iso = lambda value: value.strftime('%Y-%m-%dT%H:%M:%SZ')
dates = [(now - timedelta(days=2*i+2)).strftime('%Y-%m-%d') for i in range(40)]
team = lambda abbr, name, off, defense, pace: {
    'abbr': abbr, 'name': name, 'offRating': off, 'defRating': defense, 'pace': pace,
    'elo': 1510 if abbr == 'GSW' else 1500, 'restDays': 1, 'dataAsOf': iso(now),
    'logs': [{'date': d, 'points': round(rng.gauss(113, 11)), 'margin': round(rng.gauss(1, 12)), 'home': i % 2 == 0} for i, d in enumerate(dates)]
}
logs = []
for i, date in enumerate(dates):
    minutes = round(max(20, min(41, rng.gauss(34, 2.5))), 1)
    logs.append({'date': date, 'minutes': minutes, 'points': max(0, round(rng.gauss(27, 6))),
                 'rebounds': max(0, round(rng.gauss(4.5, 2))), 'assists': max(0, round(rng.gauss(6, 2))),
                 'threes': max(0, round(rng.gauss(4.2, 1.5))), 'steals': max(0, round(rng.gauss(1.1, .8))),
                 'blocks': max(0, round(rng.gauss(.3, .5))), 'home': i % 2 == 0,
                 'fga': 20, 'fgm': 9, 'fta': 5, 'ftm': 4, 'threeAttempts': 10, 'turnovers': 3, 'fouls': 2, 'oreb': 1})
document = {
    'synthetic': True,
    'games': [{'id': 'custom-sample', 'home': 'LAL', 'away': 'GSW', 'homeName': 'Los Angeles Lakers',
               'awayName': 'Golden State Warriors', 'startsAt': iso(now+timedelta(hours=6)),
               'status': 'upcoming', 'seasonType': 'regular', 'venue': 'SYNTHETIC example'}],
    'teams': [team('GSW', 'Golden State Warriors', 114, 113, 100), team('LAL', 'Los Angeles Lakers', 115, 114, 99)],
    'players': [{'name': 'Stephen Curry', 'team': 'GSW', 'logs': logs, 'historicalPace': 100,
                 'injury': 'Available (synthetic example)', 'dataAsOf': iso(now)}],
    'quotes': [{'gameId': 'custom-sample', 'market': 'points', 'player': 'Stephen Curry', 'team': 'GSW',
                'side': side, 'line': 26.5, 'oddsPercent': 52.38, 'book': 'SYNTHETIC example book',
                'observedAt': iso(now), 'settlement': 'full_game_including_ot'} for side in ('over','under')]
}
print(json.dumps(document, indent=2))
