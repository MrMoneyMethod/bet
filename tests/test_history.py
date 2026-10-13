#!/usr/bin/env python3
"""Offline regression tests for collection, coverage and feature export. No provider requests."""
import argparse
from contextlib import redirect_stderr, redirect_stdout
from datetime import date
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError, URLError

from history_fixtures import game_pages

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('history', ROOT / 'scripts' / 'basketball_reference_history.py')
history = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = history
spec.loader.exec_module(history)


class FakeResponse:
    status = 200

    def __init__(self, body, url):
        self.body = body.encode()
        self.url = url

    def read(self, limit):
        return self.body[:limit]

    def geturl(self):
        return self.url

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass



HOOPR_BOX_HEADER = ["game_id", "athlete_id", "athlete_display_name", "team_abbreviation", "starter",
                    "did_not_play", "minutes", "points", "field_goals_made", "field_goals_attempted",
                    "three_point_field_goals_made", "three_point_field_goals_attempted", "free_throws_made",
                    "free_throws_attempted", "offensive_rebounds", "total_rebounds", "assists", "steals",
                    "blocks", "turnovers", "personal_fouls", "game_date", "home_team_abbreviation",
                    "away_team_abbreviation", "season_type"]
HOOPR_PBP_HEADER = ["game_id", "text", "type_text", "period_number", "clock_display_value", "scoring_play",
                    "score_value", "away_score", "home_score", "athlete_id_1"]


def hoopr_rows(game_id, date_value, home, away, scorer_home=True):
    box, pbp = [], []
    for team, base in ((away, 100), (home, 200)):
        for i in range(7):
            athlete = base + i
            starter = i < 5
            points = (12 if i == 0 else 6) if starter else 3
            box.append([game_id, athlete, f"Player {athlete}", team, "TRUE" if starter else "FALSE",
                        "FALSE", "30" if starter else "10", points, 4, 8, 1, 3, 2, 2, 1, 4, 3, 1, 0, 1, 2,
                        date_value, home, away, 2])
    scorer = 200 if scorer_home else 100
    pbp.append([game_id, "Opening tip", "Jump Ball", 1, "12:00", "FALSE", 0, 0, 0, ""])
    pbp.append([game_id, "First scorer makes layup", "Layup Shot", 1, "11:40", "TRUE", 2,
                0 if scorer_home else 2, 2 if scorer_home else 0, scorer])
    pbp.append([game_id, "Reply basket", "Jump Shot", 1, "11:20", "TRUE", 2, 2, 2, 100 if scorer_home else 200])
    return box, pbp


class HoopRIngestTests(unittest.TestCase):
    def write_csv(self, directory, name, header, rows):
        import csv as csv_module
        path = Path(directory) / name
        with open(path, "w", newline="", encoding="utf-8") as handle:
            writer = csv_module.writer(handle)
            writer.writerow(header)
            writer.writerows(rows)
        return path

    def test_hoopr_csvs_build_features_with_first_basket(self):
        box, pbp = hoopr_rows("401000001", "2024-01-05", "LAL", "GSW", scorer_home=True)
        box2, pbp2 = hoopr_rows("401000002", "2024-01-06", "LAL", "GSW", scorer_home=False)
        with tempfile.TemporaryDirectory() as directory:
            box_path = self.write_csv(directory, "player_box_2024.csv", HOOPR_BOX_HEADER, box + box2)
            pbp_path = self.write_csv(directory, "play_by_play_2024.csv", HOOPR_PBP_HEADER, pbp + pbp2)
            output = Path(directory) / "history"
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(out):
                result = history.main(["hoopr", "--permission-confirmed", "--season", "2024",
                               "--player-box-csv", str(box_path), "--pbp-csv", str(pbp_path),
                               "--output", str(output), "--as-of", "2024-02-01"])
            self.assertEqual(result, 0, out.getvalue())
            archive = history.Archive(output)
            try:
                metadata = archive.status()
            finally:
                archive.db.close()
            self.assertEqual(metadata["games"], 2)
            self.assertEqual(metadata["events"], 6)
            players = {}
            for line in (output / "features.jsonl").read_text().splitlines():
                record = json.loads(line)
                if record.get("recordType") == "player":
                    players[record["feature"]["name"]] = record["feature"]
            self.assertEqual(len(players["Player 200"]["logs"]), 2)
            # History is newest-first: game 2 was won by the away scorer (100), game 1 by the home scorer (200).
            self.assertEqual([entry["win"] for entry in players["Player 200"]["firstBasketHistory"]], [False, True])
            self.assertEqual([entry["win"] for entry in players["Player 100"]["firstBasketHistory"]], [True, False])

    def test_published_hoopr_schema_and_minutes(self):
        for role, with_pbp in (("home_away", False), ("team_home_away", False), ("unused", True)):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as directory:
                box, pbp = hoopr_rows("401000001", "2024-01-05", "LAL", "GSW")
                header = [col for col in HOOPR_BOX_HEADER
                          if col not in ("home_team_abbreviation", "away_team_abbreviation")]
                rows = []
                for values in box:
                    row = dict(zip(HOOPR_BOX_HEADER, values))
                    row["minutes"] = "30:30" if row["starter"] == "TRUE" else "10:15"
                    rows.append([row[col] for col in header] + ["home" if row["team_abbreviation"] == "LAL" else "away"])
                box_path = self.write_csv(directory, "box.csv", header + [role], rows)
                arguments = ["hoopr", "--permission-confirmed", "--season", "2024",
                             "--player-box-csv", str(box_path), "--output", str(Path(directory) / "h"),
                             "--as-of", "2024-02-01"]
                if with_pbp:
                    pbp_path = self.write_csv(directory, "pbp.csv",
                                             HOOPR_PBP_HEADER + ["home_team_abbrev", "away_team_abbrev"],
                                             [row + ["LAL", "GS"] for row in pbp])
                    arguments += ["--pbp-csv", str(pbp_path)]
                output = io.StringIO()
                with redirect_stdout(output), redirect_stderr(output):
                    result = history.main(arguments)
                self.assertEqual(result, 0, output.getvalue())
                players = {}
                for line in (Path(directory) / "h" / "features.jsonl").read_text().splitlines():
                    record = json.loads(line)
                    if record["recordType"] == "player":
                        players[record["feature"]["name"]] = record["feature"]
                self.assertEqual(players["Player 200"]["logs"][0]["minutes"], 30.5)
                self.assertEqual(players["Player 206"]["logs"][0]["minutes"], 10.25)
                self.assertTrue(players["Player 200"]["logs"][0]["home"])
                self.assertFalse(players["Player 100"]["logs"][0]["home"])
                self.assertEqual(len(players["Player 200"].get("firstBasketHistory", [])), int(with_pbp))

    def test_hoopr_conflicting_home_away_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            box, _ = hoopr_rows("401000001", "2024-01-05", "LAL", "GSW")
            # Explicit home LAL contradicts the per-team role on the away rows.
            path = self.write_csv(directory, "bad.csv", HOOPR_BOX_HEADER + ["home_away"],
                                  [row + ["home"] for row in box])
            output = io.StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                result = history.main(["hoopr", "--permission-confirmed", "--season", "2024",
                                       "--player-box-csv", str(path), "--output", str(Path(directory) / "h")])
            self.assertEqual(result, 1)
            self.assertIn("Conflicting home/away", output.getvalue())

    def test_hoopr_minutes_formats(self):
        for value, expected in (("30:30", 30.5), ("00:15", .25), ("30.5", 30.5), ("NA", 0),
                                ("30:99", 0), ("nan", 0), ("inf", 0), ("30:15:20", 0), ("12x", 0)):
            with self.subTest(value=value):
                self.assertEqual(history._hoopr_minutes(value), expected)

    def test_hoopr_missing_column_fails_loudly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_csv(directory, "bad.csv", ["game_id", "athlete_id"], [["1", "2"]])
            error = io.StringIO()
            with redirect_stderr(error):
                result = history.main(["hoopr", "--permission-confirmed", "--season", "2024",
                               "--player-box-csv", str(path), "--output", str(Path(directory) / "h")])
            self.assertEqual(result, 1)
            self.assertIn("missing column", error.getvalue())


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='edgecourt-history-tests-')
        self.path = Path(self.directory.name)
        self.archive = history.Archive(self.path / 'archive')
        self.game, self.box, self.pbp, self.schedule = game_pages()

    def tearDown(self):
        self.archive.db.close()
        self.directory.cleanup()

    def complete_game(self, game_id='202604010BOS'):
        game, box, pbp, _ = game_pages(game_id)
        events, document = history.play_by_play(pbp, history.box_score(box, game))
        self.archive.discover({game_id: game})
        self.archive.finish(document, events)
        return document, events

    def test_fixed_https_path_allowlist(self):
        for url in ('http://www.basketball-reference.com/robots.txt',
                    'https://www.basketball-reference.com.evil.test/robots.txt',
                    'https://www.basketball-reference.com:443/robots.txt',
                    'https://www.basketball-reference.com/players/a/a01/gamelog/',
                    'https://www.basketball-reference.com/boxscores/202604010BOS.html?redirect=1',
                    'https://user@www.basketball-reference.com/robots.txt',
                    'https://127.0.0.1/robots.txt'):
            self.assertFalse(history.allowed_url(url), url)
        self.assertTrue(history.allowed_url(history.BASE + '/boxscores/pbp/202604010BOS.html'))
        self.assertEqual(history.canonical_team('PHO'), 'PHX')
        self.assertEqual(history.canonical_team('BRK'), 'BKN')
        self.assertEqual(history.canonical_team('CHO'), 'CHA')

    def test_month_discovery_completed_games_and_team_identity(self):
        months, games = history.schedule(self.schedule, 2026)
        self.assertEqual(months, [history.BASE + '/leagues/NBA_2026_games-april.html'])
        self.assertEqual(games[self.game['id']], self.game)
        upcoming = self.schedule.replace(f'<a href="/boxscores/{self.game["id"]}.html">Box Score</a>', '')
        self.assertEqual(history.schedule(upcoming, 2026)[1], {})
        with self.assertRaises(history.ParseError):
            history.schedule('<html>challenge, not a schedule</html>', 2026)
        with self.assertRaises(history.ParseError):
            history.schedule(self.schedule.replace('/teams/BOS/', '/teams/LAL/'), 2026)

    def test_comment_tables_complete_stats_and_dnp_is_not_zero(self):
        game, box, _, _ = game_pages(comments=True)
        result = history.box_score(box, game)
        self.assertEqual(len(result['players']), 10)
        self.assertTrue(all(p['starter'] for p in result['players']))
        self.assertEqual(result['totals']['BOS']['points'], 105)
        self.assertNotIn('dnp0001', {p['id'] for p in result['players']})
        with self.assertRaises(history.ParseError):
            history.box_score(box.replace('<td data-stat="ast">2</td>', '<td data-stat="ast"></td>', 1), game)
        with self.assertRaises(history.ParseError):
            history.box_score(box.replace('>Starters<', '>Unconfirmed<'), game)

    def test_first_field_goal_not_first_free_throw_and_tied_clocks_keep_order(self):
        box = history.box_score(self.box, self.game)
        events, result = history.play_by_play(self.pbp, box)
        self.assertEqual(events[0]['type'], 'period_start')
        self.assertEqual(events[1]['type'], 'made_free_throw')
        self.assertEqual(events[2]['type'], 'made_field_goal')
        self.assertEqual(result['firstFieldGoal']['playerId'], 'away0001')
        self.assertTrue(result['firstFieldGoalVerified'])
        self.assertEqual(events[-1]['type'], 'period_end')
        self.assertEqual([e['sequence'] for e in events], list(range(len(events))))
        self.assertEqual(events[2]['clock'], events[3]['clock'])
        self.assertLess(events[2]['sourceRow'], events[3]['sourceRow'])
        self.assertEqual(result['durationMinutes'], 48)

    def test_overtime_boundaries_and_minutes(self):
        game, box, pbp, _ = game_pages(overtime=True)
        events, result = history.play_by_play(pbp, history.box_score(box, game))
        self.assertEqual(result['durationMinutes'], 53)
        self.assertEqual(events[-1]['period'], 5)
        self.assertEqual(events[-2]['clock'], '5:00.0')
        self.assertEqual(events[-1]['elapsedSeconds'], 53*60)

    def test_partial_or_score_mismatched_pbp_is_rejected(self):
        box = history.box_score(self.box, self.game)
        for pbp in (self.pbp.replace('End of 4th quarter', 'Unknown boundary'),
                    self.pbp.replace('Start of 1st quarter', 'Unknown boundary'),
                    self.pbp.replace('104-105', '104-106'),
                    self.pbp.replace('11:58.0', '12:30.0', 1),
                    '<html><title>Access denied</title></html>'):
            with self.assertRaises(history.ParseError):
                history.play_by_play(pbp, box)
        # Preserve an incomplete-opening game's events but don't manufacture a first-FG feature.
        changed = self.pbp.replace('makes free throw 1 of 2', 'unrecognized score change', 1)
        _, result = history.play_by_play(changed, box)
        self.assertFalse(result['firstFieldGoalVerified'])
        self.assertIsNone(result['firstFieldGoal'])

    def test_sqlite_deduplicates_all_events_and_reports_partial_coverage(self):
        document, events = self.complete_game()
        self.archive.finish(document, events)
        report = self.archive.status()
        self.assertEqual(report['games'], 1)
        self.assertEqual(report['events'], len(events))
        self.assertIn('Partial', report['coverage'])
        missing = dict(self.game, id='202604020BOS', date='2026-04-02')
        self.archive.discover({missing['id']: missing})
        self.archive.mark(missing['id'], 'missing', 'PBP returned 404')
        report = self.archive.status()
        self.assertEqual(report['missingGames'], 1)
        self.assertEqual(report['missingExamples'][0]['status'], 'missing')

    def test_feature_cutoff_preserves_stat_dates_and_dated_starter_opportunities(self):
        self.complete_game()
        self.complete_game('202604020BOS')
        self.complete_game('202604030BOS')  # Target date is present in archive but excluded from features.
        report = history.export_features(self.archive, date(2026, 4, 3))
        records = [json.loads(line) for line in (self.archive.directory / 'features.jsonl').read_text().splitlines()]
        self.assertEqual(report['games'], 3)
        self.assertEqual(report['players'], 10)
        self.assertEqual(report['teams'], 2)
        self.assertEqual(records[0]['metadata']['asOf'], '2026-04-03')
        for row in records[1:]:
            feature = row['feature']
            self.assertEqual(len(feature['logs']), 2)
            self.assertEqual(feature['dataAsOf'], '2026-04-02T23:59:59Z')
            self.assertEqual(feature['logs'][0]['date'], '2026-04-02')
            if row['recordType'] == 'player':
                self.assertFalse(feature['starterConfirmed'])
                self.assertEqual(len(feature['firstBasketHistory']), 2)
                wins = sum(o['win'] for o in feature['firstBasketHistory'])
                self.assertEqual(wins, 2 if feature['basketballReferenceId'] == 'away0001' else 0)
        self.assertFalse((self.archive.directory / 'features.tmp').exists())

    def test_schedule_playoff_markers_survive_plain_box_score_titles(self):
        schedule = self.schedule.replace('<tbody>', '<tbody><tr><th colspan="10">Playoffs</th></tr>', 1)
        _, games = history.schedule(schedule, 2026)
        self.assertEqual(games[self.game['id']]['seasonType'], 'playoffs')
        result = history.box_score(self.box, games[self.game['id']])
        self.assertEqual(result['seasonType'], 'playoffs')
        self.archive.discover(games)
        self.assertEqual(self.archive.db.execute('SELECT season_type FROM games').fetchone()[0], 'playoffs')

    def test_low_minute_logs_and_blank_source_rows_stay_missing_not_zero(self):
        box = self.box.replace('<td data-stat="mp">48:00</td>', '<td data-stat="mp">0:01</td>', 1)
        parsed = history.box_score(box, self.game)
        self.assertLess(parsed['players'][0]['log']['minutes'], .1)
        # A source visual gap before the first FG conservatively disables that feature.
        blank = '<tr><td>11:58.5</td><td></td><td></td><td></td><td></td><td></td></tr>'
        pbp = self.pbp.replace('<tr><td>11:58.0</td>', blank+'<tr><td>11:58.0</td>', 1)
        events, result = history.play_by_play(pbp, parsed)
        missing = next(e for e in events if e['type']=='empty_source_row')
        self.assertIsNone(missing['awayScore'])
        self.assertFalse(result['firstFieldGoalVerified'])
        self.archive.discover({self.game['id']: self.game})
        self.archive.finish(result, events)
        history.export_features(self.archive, date(2026,4,3))
        records = [json.loads(line) for line in (self.archive.directory/'features.jsonl').read_text().splitlines()[1:]]
        self.assertFalse(any(r['recordType']=='player' and r['feature']['basketballReferenceId']=='away0001' for r in records))

    def test_playoff_vectors_not_blended_into_regular_season_stats(self):
        game = self.game
        box = self.box.replace('Fixture NBA regular game', '2026 NBA Finals Game 5 - Fixture')
        events, document = history.play_by_play(self.pbp, history.box_score(box, game))
        self.archive.discover({game['id']: game})
        self.archive.finish(document, events)
        report = history.export_features(self.archive, date(2026, 4, 3))
        self.assertEqual(report['teams'], 0)
        self.assertEqual(report['players'], 10)  # Still provides first-FG observations from historical starts.
        for line in (self.archive.directory / 'features.jsonl').read_text().splitlines()[1:]:
            feature = json.loads(line)['feature']
            self.assertEqual(feature['logs'], [])
            self.assertEqual(len(feature['firstBasketHistory']), 1)

    def test_saved_page_ingestion_without_network_and_duplicate_runs(self):
        root = self.path / 'saved'
        (root / 'boxscores' / 'pbp').mkdir(parents=True)
        (root / 'leagues').mkdir()
        (root / 'boxscores' / f'{self.game["id"]}.html').write_text(self.box)
        (root / 'boxscores' / 'pbp' / f'{self.game["id"]}.html').write_text(self.pbp)
        (root / 'leagues' / 'NBA_2026_games-april.html').write_text(self.schedule)
        args = argparse.Namespace(saved_html=root, season=2026)
        with patch.object(history, 'build_opener', side_effect=AssertionError('No network allowed')):
            history.ingest(self.archive, args)
            history.ingest(self.archive, args)
        self.assertEqual(self.archive.status()['games'], 1)
        self.assertEqual(self.archive.cached_page(history.BASE + f'/boxscores/{self.game["id"]}.html'), (200, self.box))
        self.assertIn('Partial', self.archive.status()['coverage'])

    def test_robots_fail_closed_and_crawl_delay(self):
        client = history.Client(self.archive, delay=0)
        robots = 'User-agent: *\nDisallow: /boxscores/\nCrawl-delay: 8\n'
        client.opener = Mock()
        client.opener.open.return_value = FakeResponse(robots, history.BASE + '/robots.txt')
        client.check_robots()
        self.assertEqual(client.delay, 8)
        with self.assertRaises(history.AccessStopped):
            client.get(history.BASE + '/boxscores/pbp/202604010BOS.html')
        self.assertEqual(client.opener.open.call_count, 1)
        client.opener.open.return_value = FakeResponse('<html>challenge</html>', history.BASE + '/robots.txt')
        with patch.object(history.time, 'sleep'):
            with self.assertRaises(history.AccessStopped):
                client.check_robots()

    def test_robots_wildcards_anchors_and_specific_agent_groups(self):
        policy = history.RobotsPolicy()
        policy.parse("User-agent: GPTBot\nDisallow: /\n\nUser-agent: *\nDisallow: /boxscores/*\nAllow: /boxscores/pbp/202604010BOS.html$\nCrawl-delay: 3.5\n".splitlines())
        self.assertFalse(policy.can_fetch(history.AGENT, history.BASE+'/boxscores/202604010BOS.html'))
        self.assertTrue(policy.can_fetch(history.AGENT, history.BASE+'/boxscores/pbp/202604010BOS.html'))
        self.assertFalse(policy.can_fetch(history.AGENT, history.BASE+'/boxscores/pbp/202604020BOS.html'))
        self.assertFalse(policy.can_fetch('GPTBot', history.BASE+'/leagues/NBA_2026_games.html'))
        self.assertEqual(policy.crawl_delay(history.AGENT), 3.5)
        policy.parse("User-agent: *\nDisallow: /\n\nUser-agent: EdgecourtHistory\nDisallow: /boxscores/\nAllow: /boxscores/pbp/\nCrawl-delay: 4\n".splitlines())
        self.assertTrue(policy.can_fetch(history.AGENT, history.BASE+'/leagues/NBA_2026_games.html'))
        self.assertTrue(policy.can_fetch(history.AGENT, history.BASE+'/boxscores/pbp/202604010BOS.html'))
        self.assertFalse(policy.can_fetch(history.AGENT, history.BASE+'/boxscores/202604010BOS.html'))
        self.assertEqual(policy.crawl_delay(history.AGENT), 4)

    def test_rate_spacing_cache_integrity_and_no_access_denial_retries(self):
        client = history.Client(self.archive)
        client.opener = Mock()
        robots_url = history.BASE + '/robots.txt'
        game_url = history.BASE + '/boxscores/pbp/202604010BOS.html'
        client.opener.open.side_effect = [FakeResponse('User-agent: *\nDisallow:\nCrawl-delay: 3\n', robots_url),
                                          FakeResponse(self.pbp, game_url)]
        with patch.object(history.time, 'sleep') as sleep:
            client.check_robots()
            self.assertEqual(client.get(game_url), self.pbp)
            self.assertEqual(client.get(game_url), self.pbp)
            self.assertEqual(client.opener.open.call_count, 2)
            self.assertGreaterEqual(sleep.call_args[0][0], 3.0)
        url = history.BASE + '/boxscores/202604010BOS.html'
        for code in (403, 429, 503, 302):
            client.opener.open.side_effect = HTTPError(url, code, 'Blocked', {}, None)
            with patch.object(history.time, 'sleep'):
                with self.assertRaises(history.AccessStopped):
                    client.get(url)
        client.opener.open.side_effect = HTTPError(url, 404, 'Missing', {}, None)
        with patch.object(history.time, 'sleep'):
            self.assertIsNone(client.get(url))
            previous_calls = client.opener.open.call_count
            self.assertIsNone(client.get(url))
            self.assertEqual(client.opener.open.call_count, previous_calls)
        with self.archive.db:
            self.archive.db.execute('UPDATE pages SET sha256=? WHERE url=?', ('wrong', game_url))
        with self.assertRaises(history.ParseError):
            self.archive.cached_page(game_url)

    def test_resumable_crawl_and_game_limit_keep_gaps_visible(self):
        args = argparse.Namespace(start_season=2026, end_season=2026, delay=3.1, refresh_schedules=False,
                                  retry_missing=False, max_games=1)
        second_game, box2, pbp2, schedule2 = game_pages('202604020BOS')
        pages = {history.BASE + '/leagues/NBA_2026_games.html': self.schedule,
                 history.BASE + '/leagues/NBA_2026_games-april.html': schedule2,
                 history.BASE + f'/boxscores/{self.game["id"]}.html': self.box,
                 history.BASE + f'/boxscores/pbp/{self.game["id"]}.html': self.pbp,
                 history.BASE + f'/boxscores/{second_game["id"]}.html': box2,
                 history.BASE + f'/boxscores/pbp/{second_game["id"]}.html': pbp2}
        with patch.object(history.Client, 'check_robots'), patch.object(history.Client, 'get', side_effect=lambda url, force=False: pages[url]) as get:
            with patch('sys.stdout', new_callable=io.StringIO):
                history.crawl(self.archive, args)
            self.assertEqual(self.archive.status()['games'], 1)
            self.assertEqual(self.archive.status()['missingGames'], 1)
            get.reset_mock()
            with patch('sys.stdout', new_callable=io.StringIO):
                history.crawl(self.archive, args)
            self.assertEqual(self.archive.status()['games'], 2)
            self.assertEqual(self.archive.status()['missingGames'], 0)
            self.assertFalse(any(self.game['id'] in c.args[0] for c in get.call_args_list))
            self.assertIn('All discovered', self.archive.status()['coverage'])

    def test_permission_and_single_writer_gates(self):
        output = self.path / 'no-permission'
        with patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit) as error:
                history.main(['crawl', '--output', str(output)])
        self.assertEqual(error.exception.code, 2)
        self.assertFalse(output.exists())
        with history.single_writer(self.path):
            with self.assertRaises(history.AccessStopped):
                with history.single_writer(self.path):
                    pass
        self.assertFalse((self.path / 'collector.lock').exists())


if __name__ == '__main__':
    unittest.main()
