#!/usr/bin/env python3
"""Authorized, resumable NBA play-by-play collection. Python standard library only.

Raw pages and every normalized event live outside Git in a local SQLite archive.
This tool does not bypass challenges, redirects, robots exclusions or rate limits.
No crawl has been executed or historical dataset bundled by implementing this tool.
See docs/HISTORICAL_DATA.md for permissions, coverage and model limitations.
"""
from __future__ import annotations

import argparse
import csv
from collections import defaultdict, deque
from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import date, datetime, timedelta, timezone
import gzip
import hashlib
from html.parser import HTMLParser
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import sys
import time
import unicodedata
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

BASE = "https://www.basketball-reference.com"
AGENT = "EdgecourtHistory"
USER_AGENT = "EdgecourtHistory/0.1 (+https://github.com/MrMoneyMethod/bet; authorized local research)"
MAX_PAGE = 16 * 1024 * 1024
GAME_ID = r"\d{9}[A-Z]{3}"
GAME_PATH = re.compile(rf"^/boxscores/(?P<pbp>pbp/)?(?P<id>{GAME_ID})\.html$")
SEASON_PATH = re.compile(r"^/leagues/NBA_(\d{4})_games(?:-[a-z]+)?\.html$")
PLAYER_PATH = re.compile(r"^/players/[a-z]/([a-z0-9]+)\.html$")
TEAM_PATH = re.compile(r"^/teams/([A-Z]{3})/\d{4}\.html$")
TEAM_ALIASES = {"PHO": "PHX", "BRK": "BKN", "NJN": "BKN", "CHO": "CHA", "CHH": "CHA", "CHA": "CHA",
                "NOH": "NOP", "NOK": "NOP", "SEA": "OKC", "VAN": "MEM", "WSB": "WAS"}
NBA_TEAMS = set("ATL BOS BKN CHA CHI CLE DAL DEN DET GSW HOU IND LAC LAL MEM MIA MIL MIN NOP NYK OKC ORL PHI PHX POR SAC SAS TOR UTA WAS".split())
STATS = {"pts": "points", "trb": "rebounds", "ast": "assists", "fg3": "threes",
         "stl": "steals", "blk": "blocks", "fg": "fgm", "fga": "fga", "ft": "ftm",
         "fta": "fta", "fg3a": "threeAttempts", "orb": "oreb", "tov": "turnovers", "pf": "fouls"}


class ParseError(ValueError):
    pass


class AccessStopped(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def canonical_team(team: str) -> str:
    result = TEAM_ALIASES.get(team, team)
    if result not in NBA_TEAMS:
        raise ParseError(f"Unsupported NBA team: {team}")
    return result


def game_date(game_id: str) -> str:
    if not re.fullmatch(GAME_ID, game_id):
        raise ParseError("Invalid Basketball Reference game ID")
    return datetime.strptime(game_id[:8], "%Y%m%d").date().isoformat()


def allowed_url(url: str) -> bool:
    parts = urlsplit(url)
    return (parts.scheme == "https" and parts.netloc == "www.basketball-reference.com"
            and not parts.query and not parts.fragment
            and (parts.path == "/robots.txt" or bool(SEASON_PATH.fullmatch(parts.path))
                 or bool(GAME_PATH.fullmatch(parts.path))))


def normalized_name(name: str) -> str:
    plain = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    return re.sub(r"[^a-z0-9]", "", plain.lower())


@dataclass
class Cell:
    attrs: dict = field(default_factory=dict)
    text: str = ""
    links: list = field(default_factory=list)


@dataclass
class Row:
    attrs: dict = field(default_factory=dict)
    section: str = ""
    cells: list[Cell] = field(default_factory=list)

    def stat(self, name: str) -> Cell | None:
        return next((c for c in self.cells if c.attrs.get("data-stat") == name), None)


class Page(HTMLParser):
    """Data-only table reader, including Sports Reference comment-wrapped tables."""
    def __init__(self, html: str):
        super().__init__(convert_charrefs=True)
        self.tables: dict[str, list[Row]] = {}
        self.links: list[dict] = []
        self.title = ""
        self._table = None
        self._row = None
        self._cell = None
        self._link = None
        self._section = ""
        self._title = False
        self.feed(html)
        self.close()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "title":
            self._title = True
        elif tag == "table":
            self._table = attrs.get("id", "")
            self.tables.setdefault(self._table, [])
        elif tag in ("thead", "tbody", "tfoot"):
            self._section = tag
        elif tag == "tr" and self._table is not None:
            self._row = Row(attrs=attrs, section=self._section)
            self.tables[self._table].append(self._row)
        elif tag in ("td", "th") and self._row is not None:
            self._cell = Cell(attrs=attrs)
            self._row.cells.append(self._cell)
        elif tag == "a":
            self._link = {"href": attrs.get("href", ""), "text": ""}
            self.links.append(self._link)
            if self._cell is not None:
                self._cell.links.append(self._link)
        elif tag == "br" and self._cell is not None:
            self._cell.text += " "

    def handle_endtag(self, tag):
        if tag == "title":
            self._title = False
        elif tag == "table":
            self._table = self._row = self._cell = None
        elif tag == "tr":
            self._row = self._cell = None
        elif tag in ("td", "th"):
            self._cell = None
        elif tag == "a":
            self._link = None
        elif tag in ("thead", "tbody", "tfoot"):
            self._section = ""

    def handle_data(self, data):
        if self._title:
            self.title += data
        if self._cell is not None:
            self._cell.text += data
        if self._link is not None:
            self._link["text"] += data

    def handle_comment(self, data):
        if "<table" in data.lower():
            child = Page(data)
            self.tables.update(child.tables)
            self.links.extend(child.links)


def cell_text(cell: Cell | None) -> str:
    return " ".join(cell.text.split()) if cell else ""


def player_link(cell: Cell) -> dict | None:
    for link in cell.links:
        match = PLAYER_PATH.fullmatch(urlsplit(link["href"]).path)
        if match:
            return {"id": match[1], "name": " ".join(link["text"].split())}
    return None


def schedule(html: str, season: int) -> tuple[list[str], dict[str, dict]]:
    page = Page(html)
    if "schedule" not in page.tables:
        raise ParseError("Schedule table missing; not treating a challenge/changed schema as empty history")
    months = sorted({urljoin(BASE, a["href"]) for a in page.links
                     if re.fullmatch(rf"/leagues/NBA_{season}_games-[a-z]+\.html", urlsplit(a["href"]).path)})
    games = {}
    phase = "regular"
    for row in page.tables["schedule"]:
        if " ".join(cell_text(c) for c in row.cells).strip() == "Playoffs":
            phase = "playoffs"
            continue
        box = row.stat("box_score_text")
        if not box:
            continue
        for link in box.links:
            match = GAME_PATH.fullmatch(urlsplit(link["href"]).path)
            if not match or match["pbp"]:
                continue
            teams = {}
            for key in ("visitor_team_name", "home_team_name"):
                cell = row.stat(key)
                team_match = next((TEAM_PATH.fullmatch(urlsplit(a["href"]).path)
                                   for a in (cell.links if cell else [])
                                   if TEAM_PATH.fullmatch(urlsplit(a["href"]).path)), None)
                if not team_match:
                    raise ParseError("Completed schedule row has no unambiguous team IDs")
                teams["away" if key.startswith("visitor") else "home"] = canonical_team(team_match[1])
            if teams["home"] != canonical_team(match["id"][-3:]) or teams["away"] == teams["home"]:
                raise ParseError("Schedule/game ID team mismatch")
            notes = cell_text(row.stat("game_remarks")) + " " + cell_text(row.stat("notes"))
            game_phase = "cup_final" if re.search(r"(?:nba cup|in-season tournament).*(?:final|championship)", notes, re.I) else phase
            games[match["id"]] = {"id": match["id"], "season": season,
                                   "date": game_date(match["id"]), "seasonType": game_phase, **teams}
    return months, games


def stat_int(row: Row, key: str) -> int:
    value = cell_text(row.stat(key))
    if not re.fullmatch(r"\d+", value):
        raise ParseError(f"Missing/malformed box-score statistic: {key}")
    result = int(value)
    if not 0 <= result <= 300:
        raise ParseError(f"Out-of-range box-score statistic: {key}")
    return result


def minutes(text: str) -> float:
    if re.fullmatch(r"\d+:\d{2}", text):
        whole, seconds = map(int, text.split(":"))
        if seconds >= 60:
            raise ParseError("Invalid box-score minutes")
        return whole + seconds / 60
    if re.fullmatch(r"\d+(?:\.\d+)?", text):
        return float(text)
    raise ParseError("Missing/malformed played minutes")


def box_score(html: str, game: dict) -> dict:
    page = Page(html)
    tables = {match[1]: rows for key, rows in page.tables.items()
              if (match := re.fullmatch(r"box-([A-Z]{3})-game-basic", key))}
    if len(tables) != 2:
        raise ParseError("Expected both complete full-game basic box scores")
    if not game.get("away"):
        game = dict(game)
        game["home"] = canonical_team(game["id"][-3:])
        game["away"] = next((canonical_team(t) for t in tables if canonical_team(t) != game["home"]), "")
    players, totals = [], {}
    for raw_team, rows in tables.items():
        team = canonical_team(raw_team)
        if team not in (game["home"], game["away"]):
            raise ParseError("Box-score teams do not match schedule")
        has_starters = any(cell_text(row.stat("player")) == "Starters" for row in rows)
        if not has_starters:
            raise ParseError("Box-score starter section missing; never infer starters from scoring rates")
        player_index = 0
        for row in rows:
            cell = row.stat("player")
            if not cell:
                continue
            identity = player_link(cell)
            if identity:
                starter = player_index < 5
                player_index += 1
                reason = cell_text(row.stat("reason"))
                if reason or "Did Not" in " ".join(c.text for c in row.cells) or "Not With Team" in cell.text:
                    if starter:
                        raise ParseError("Historical starter has no played stats")
                    continue  # DNP is not a zero-stat played game.
                log = {"date": game["date"], "gameId": game["id"], "home": team == game["home"],
                       "minutes": minutes(cell_text(row.stat("mp")))}
                if not 0 <= log["minutes"] <= 70:
                    raise ParseError("Unsupported player minutes")
                log.update({field_name: stat_int(row, raw) for raw, field_name in STATS.items()})
                players.append({**identity, "team": team, "starter": starter, "log": log})
            elif cell_text(cell) == "Team Totals":
                totals[team] = {field_name: stat_int(row, raw) for raw, field_name in STATS.items()}
    if len(totals) != 2 or any(sum(p["starter"] for p in players if p["team"] == t) != 5 for t in totals):
        raise ParseError("Incomplete totals or historical starting lineup")
    if len({p["id"] for p in players}) != len(players):
        raise ParseError("Duplicate historical player ID")
    for team, total in totals.items():
        # Points must agree with official team totals, not be silently reconstructed from incomplete PBP.
        if sum(p["log"]["points"] for p in players if p["team"] == team) != total["points"]:
            raise ParseError("Played-player points disagree with team total")
    title = page.title.lower()
    if re.search(r"nba cup final|in-season tournament (?:championship|final)", title):
        season_type = "cup_final"
    elif re.search(r"nba finals|conference (?:finals|semifinals)|first round|playoffs|play-in", title):
        season_type = "playoffs"
    else:
        season_type = game.get("seasonType", "regular")
    return {**game, "seasonType": season_type, "players": players, "totals": totals}


def event_type(description: str) -> str:
    if re.search(r"\bmakes [23]-pt\b", description):
        return "made_field_goal"
    if re.search(r"\bmakes free throw\b", description):
        return "made_free_throw"
    if "misses" in description:
        return "missed_shot"
    if "rebound" in description.lower():
        return "rebound"
    if "turnover" in description.lower():
        return "turnover"
    if "enters the game" in description:
        return "substitution"
    if "foul" in description.lower():
        return "foul"
    return "other"


def play_by_play(html: str, box: dict) -> tuple[list[dict], dict]:
    rows = Page(html).tables.get("pbp")
    if not rows:
        raise ParseError("Play-by-play table missing; access/challenge pages are not historical events")
    period = 0
    events = []
    starts, ends = set(), set()
    scores = (0, 0)
    last_elapsed = -1.0
    first_fg = None
    opening_verified = True
    known_players = {p["id"]: p for p in box["players"]}
    for source_row, row in enumerate(rows):
        if not row.cells:
            continue
        label = " ".join(cell_text(c) for c in row.cells)
        marker = re.fullmatch(r"([1-4])(?:st|nd|rd|th) Q", label)
        overtime = re.fullmatch(r"(?:(\d+)(?:st|nd|rd|th) )?OT", label)
        if marker:
            period = int(marker[1])
            continue
        if overtime:
            period = 4 + int(overtime[1] or "1")
            continue
        clock = cell_text(row.cells[0])
        if not re.fullmatch(r"\d{1,2}:\d{2}(?:\.\d+)?", clock):
            continue  # Headers, not a guessed clock.
        clock_minutes, clock_seconds = clock.split(":")
        remaining = int(clock_minutes) * 60 + float(clock_seconds)
        if period == 0:
            # Some saved schemas omit quarter headings but keep explicit start rows.
            explicit = re.search(r"Start of (\d+)(?:st|nd|rd|th) quarter", label)
            if explicit:
                period = int(explicit[1])
        duration = 720 if period <= 4 else 300
        if period < 1 or remaining > duration or float(clock_seconds) >= 60:
            raise ParseError("Unknown period or invalid play-by-play clock")
        elapsed = (period - 1) * 720 - remaining + 720 if period <= 4 else 2880 + (period - 5) * 300 + 300 - remaining
        if elapsed + 0.001 < last_elapsed:
            raise ParseError("Play-by-play clock runs backwards; preserve source order rather than sort ties")
        last_elapsed = elapsed
        if "Start of" in label:
            if remaining != duration:
                raise ParseError("Truncated period opening")
            starts.add(period)
        if "End of" in label:
            if remaining != 0:
                raise ParseError("Truncated period ending")
            ends.add(period)
        if len(row.cells) != 6:
            description = " ".join(cell_text(c) for c in row.cells[1:])
            events.append({"sequence": len(events), "sourceRow": source_row, "period": period,
                           "clock": clock, "elapsedSeconds": elapsed, "team": None, "side": None,
                           "description": description,
                           "players": [{"id": match[1], "label": " ".join(link["text"].split())}
                                       for cell in row.cells[1:] for link in cell.links
                                       if (match := PLAYER_PATH.fullmatch(urlsplit(link["href"]).path))],
                           "type": "period_start" if "Start of" in label else "period_end" if "End of" in label else "other",
                           "awayScore": scores[0], "homeScore": scores[1]})
            continue
        score_match = re.fullmatch(r"(\d+)\s*[-–]\s*(\d+)", cell_text(row.cells[3]))
        if not score_match:
            if cell_text(row.cells[1]) or cell_text(row.cells[5]):
                raise ParseError("Scored play-by-play row has no cumulative away/home scores")
            events.append({"sequence": len(events), "sourceRow": source_row, "period": period,
                           "clock": clock, "elapsedSeconds": elapsed, "team": None, "side": None,
                           "description": "", "players": [], "type": "empty_source_row",
                           "awayScore": None, "homeScore": None})
            if not first_fg:
                opening_verified = False
            continue
        next_scores = tuple(map(int, score_match.groups()))
        for side, index, score_index in (("away", 1, 0), ("home", 5, 1)):
            cell = row.cells[index]
            description = cell_text(cell)
            if not description:
                continue
            kind = event_type(description)
            identities = []
            for link in cell.links:
                match = PLAYER_PATH.fullmatch(urlsplit(link["href"]).path)
                if match:
                    identities.append({"id": match[1], "label": " ".join(link["text"].split())})
            event = {"sequence": len(events), "sourceRow": source_row, "period": period, "clock": clock,
                     "elapsedSeconds": elapsed, "team": box[side], "side": side,
                     "description": description, "type": kind, "players": identities,
                     "awayScore": next_scores[0], "homeScore": next_scores[1]}
            events.append(event)
            delta = next_scores[score_index] - scores[score_index]
            expected = int(re.search(r"makes ([23])-pt", description)[1]) if kind == "made_field_goal" else 1 if kind == "made_free_throw" else 0
            if not first_fg and delta != expected:
                opening_verified = False  # Don't infer a first-FG winner across unexplained score changes.
            if kind == "made_field_goal" and not first_fg:
                if not identities or identities[0]["id"] not in known_players:
                    opening_verified = False
                elif known_players[identities[0]["id"]]["team"] != box[side]:
                    opening_verified = False
                else:
                    first_fg = {"playerId": identities[0]["id"], "team": box[side],
                                "sequence": event["sequence"], "period": period, "clock": clock}
        if any(n < p for n, p in zip(next_scores, scores)) and not first_fg:
            opening_verified = False
        scores = next_scores
    if not events or period < 4 or starts != set(range(1, period + 1)) or ends != starts:
        raise ParseError("Missing period starts/ends; partial play-by-play is not a complete game")
    expected_score = (box["totals"][box["away"]]["points"], box["totals"][box["home"]]["points"])
    if scores != expected_score or scores[0] == scores[1]:
        raise ParseError("Final play-by-play score disagrees with the paired box score")
    result = dict(box)
    result["periods"] = period
    result["durationMinutes"] = 48 + max(0, period - 4) * 5
    result["firstFieldGoal"] = first_fg if opening_verified else None
    result["firstFieldGoalVerified"] = bool(first_fg and opening_verified)
    return events, result


class Archive:
    def __init__(self, directory: Path):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(directory / "history.sqlite3")
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys = ON")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS pages(url TEXT PRIMARY KEY, status INTEGER NOT NULL,
                path TEXT, sha256 TEXT, fetched_at TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS seasons(season INTEGER PRIMARY KEY,
                schedule_complete INTEGER NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '');
            CREATE TABLE IF NOT EXISTS games(id TEXT PRIMARY KEY, season INTEGER NOT NULL,
                date TEXT NOT NULL, home TEXT NOT NULL, away TEXT NOT NULL,
                season_type TEXT NOT NULL DEFAULT 'regular', status TEXT NOT NULL DEFAULT 'pending', error TEXT NOT NULL DEFAULT '', document TEXT);
            CREATE TABLE IF NOT EXISTS events(game_id TEXT NOT NULL REFERENCES games(id),
                sequence INTEGER NOT NULL, event TEXT NOT NULL, PRIMARY KEY(game_id, sequence));
            CREATE INDEX IF NOT EXISTS games_date ON games(date);
            CREATE INDEX IF NOT EXISTS games_season ON games(season);
        """)
        if "season_type" not in {r[1] for r in self.db.execute("PRAGMA table_info(games)")}:
            self.db.execute("ALTER TABLE games ADD COLUMN season_type TEXT NOT NULL DEFAULT 'regular'")
        existing = self.meta("schemaVersion")
        if existing not in (None, 1):
            raise ValueError("Unsupported archive schema; use a separate output directory")
        self.set_meta("schemaVersion", 1)

    def meta(self, key):
        row = self.db.execute("SELECT value FROM metadata WHERE key=?", (key,)).fetchone()
        return json.loads(row[0]) if row else None

    def set_meta(self, key, value):
        with self.db:
            self.db.execute("INSERT OR REPLACE INTO metadata VALUES (?,?)", (key, json.dumps(value)))

    def cache_page(self, url: str, status: int, body: bytes = b""):
        digest = hashlib.sha256(body).hexdigest() if body else ""
        relative = f"pages/{digest}.html.gz" if body else ""
        if body:
            target = self.directory / relative
            target.parent.mkdir(exist_ok=True)
            temp = target.with_suffix(".tmp")
            with gzip.open(temp, "wb") as output:
                output.write(body)
            os.replace(temp, target)
        with self.db:
            self.db.execute("INSERT OR REPLACE INTO pages VALUES (?,?,?,?,?)",
                            (url, status, relative, digest, utc_now()))

    def cached_page(self, url: str) -> tuple[int, str] | None:
        row = self.db.execute("SELECT * FROM pages WHERE url=?", (url,)).fetchone()
        if not row:
            return None
        if row["status"] == 404:
            return 404, ""
        if row["status"] != 200:
            return None
        try:
            with gzip.open(self.directory / row["path"], "rb") as source:
                body = source.read(MAX_PAGE + 1)
            if len(body) > MAX_PAGE or hashlib.sha256(body).hexdigest() != row["sha256"]:
                raise ParseError("Cached page checksum or size mismatch")
            return 200, body.decode("utf-8")
        except (OSError, UnicodeDecodeError) as error:
            raise ParseError(f"Cannot read cached source page: {error}") from error

    def discover(self, games: dict[str, dict]):
        with self.db:
            self.db.executemany("INSERT INTO games(id,season,date,home,away,season_type) VALUES (?,?,?,?,?,?) "
                                "ON CONFLICT(id) DO UPDATE SET season_type=excluded.season_type",
                                [(g["id"], g["season"], g["date"], g["home"], g["away"], g.get("seasonType", "regular")) for g in games.values()])

    def mark(self, game_id: str, status: str, error: str):
        with self.db:
            self.db.execute("UPDATE games SET status=?,error=? WHERE id=?", (status, error[:1000], game_id))

    def finish(self, game: dict, events: list[dict]):
        game["sourceUrl"] = f"{BASE}/boxscores/pbp/{game['id']}.html"
        game["boxScoreUrl"] = f"{BASE}/boxscores/{game['id']}.html"
        with self.db:
            self.db.execute("DELETE FROM events WHERE game_id=?", (game["id"],))
            self.db.executemany("INSERT INTO events VALUES (?,?,?)",
                                [(game["id"], event["sequence"], json.dumps(event, ensure_ascii=False)) for event in events])
            self.db.execute("UPDATE games SET status='complete',error='',document=?,season_type=? WHERE id=?",
                            (json.dumps(game, ensure_ascii=False), game["seasonType"], game["id"]))

    def finish_external(self, game: dict, events: list[dict]):
        """Archive a game document built from an external open dataset (hoopR)."""
        with self.db:
            self.db.execute("DELETE FROM events WHERE game_id=?", (game["id"],))
            self.db.executemany("INSERT INTO events VALUES (?,?,?)",
                                [(game["id"], event["sequence"], json.dumps(event, ensure_ascii=False)) for event in events])
            self.db.execute("UPDATE games SET status='complete',error='',document=?,season_type=? WHERE id=?",
                            (json.dumps(game, ensure_ascii=False), game["seasonType"], game["id"]))

    def status(self):
        summary = self.db.execute("SELECT COUNT(*) games, MIN(date) first_date, MAX(date) last_date FROM games WHERE status='complete'").fetchone()
        pending = self.db.execute("SELECT COUNT(*) FROM games WHERE status!='complete'").fetchone()[0]
        seasons = []
        for row in self.db.execute("SELECT * FROM seasons ORDER BY season"):
            counts = self.db.execute("SELECT COUNT(*) total, SUM(status='complete') complete FROM games WHERE season=?", (row["season"],)).fetchone()
            seasons.append({"season": row["season"], "scheduleComplete": bool(row["schedule_complete"]),
                            "discoveredGames": counts["total"], "archivedGames": counts["complete"] or 0,
                            "error": row["error"]})
        requested = self.meta("requestedSeasons") or []
        checked = {s["season"] for s in seasons if s["scheduleComplete"]}
        complete = bool(requested) and set(requested).issubset(checked) and pending == 0
        errors = [dict(r) for r in self.db.execute("SELECT id,status,error FROM games WHERE status!='complete' ORDER BY date DESC LIMIT 20")]
        return {"games": summary["games"], "events": self.db.execute("SELECT COUNT(*) FROM events").fetchone()[0],
                "missingGames": pending, "firstDate": summary["first_date"] or "",
                "lastDate": summary["last_date"] or "", "seasonCoverage": seasons,
                "coverage": "All discovered completed games in requested seasons (not independently verified)" if complete else "Partial / unverified season discovery",
                "lastError": self.meta("lastError") or "", "missingExamples": errors}


class NoRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class RobotsPolicy:
    """REP agent groups, longest matching allow/disallow rules, * wildcards and $ anchors."""
    def __init__(self):
        self.groups = []

    def parse(self, lines):
        agents, rules, delay = [], [], 0.0
        has_directives = False

        def finish():
            nonlocal agents, rules, delay, has_directives
            if agents:
                self.groups.append((agents, rules, delay))
            agents, rules, delay, has_directives = [], [], 0.0, False

        self.groups = []
        for raw in lines:
            line = raw.split("#", 1)[0].strip()
            if not line:
                finish()
                continue
            if ":" not in line:
                continue
            key, value = (part.strip() for part in line.split(":", 1))
            key = key.lower()
            if key == "user-agent":
                if has_directives:
                    finish()
                if value:
                    agents.append(value.lower())
            elif agents:
                has_directives = True
                if key in ("allow", "disallow") and value:
                    rules.append((key == "allow", value))
                elif key == "crawl-delay":
                    try:
                        parsed = float(value)
                        if math.isfinite(parsed) and parsed >= 0:
                            delay = max(delay, parsed)
                    except ValueError:
                        pass
        finish()
        if not self.groups:
            raise AccessStopped("Robots policy has no readable agent groups; failing closed")

    def matched_groups(self, agent):
        product = agent.lower().split("/", 1)[0]
        matches = []
        for agents, rules, delay in self.groups:
            scores = [0 if a == "*" else len(a) for a in agents if a == "*" or product.startswith(a)]
            if scores:
                matches.append((max(scores), rules, delay))
        best = max((m[0] for m in matches), default=-1)
        return [m for m in matches if m[0] == best]

    def can_fetch(self, agent, url):
        path = urlsplit(url).path
        matches = []
        for _, rules, _ in self.matched_groups(agent):
            for allow, pattern in rules:
                anchor = pattern.endswith("$")
                body = pattern[:-1] if anchor else pattern
                regex = "^" + re.escape(body).replace(r"\*", ".*") + ("$" if anchor else "")
                if re.search(regex, path):
                    matches.append((len(body.replace("*", "")), allow))
        # Allow wins an equal-specificity tie. With no applicable rule, public paths are allowed.
        return max(matches, default=(0, True))[1]

    def crawl_delay(self, agent):
        return max((m[2] for m in self.matched_groups(agent)), default=0)


class Client:
    def __init__(self, archive: Archive, delay: float = 3.1):
        self.archive = archive
        self.delay = max(3.1, delay)
        self.opener = build_opener(NoRedirects())
        self.robots = RobotsPolicy()
        self.robots_checked = False
        self.requests = 0

    def _download(self, url: str) -> str | None:
        if not allowed_url(url):
            raise AccessStopped("URL is outside the fixed Basketball Reference path allowlist")
        last = self.archive.meta("lastRequestAt") or 0
        wait = self.delay - (time.time() - last)
        if wait > 0:
            time.sleep(wait)
        self.archive.set_meta("lastRequestAt", time.time())
        self.requests += 1
        try:
            with self.opener.open(Request(url, headers={"User-Agent": USER_AGENT, "Accept": "text/html,text/plain"}), timeout=30) as response:
                if response.status != 200:
                    raise AccessStopped(f"Stopped on HTTP {response.status}: {url}")
                body = response.read(MAX_PAGE + 1)
                if len(body) > MAX_PAGE:
                    raise AccessStopped("Source response exceeds 16 MB")
                if response.geturl() != url:
                    raise AccessStopped("Redirected source is not permitted")
                html = body.decode("utf-8")
                if re.search(r"<title[^>]*>\s*(?:just a moment|access denied|attention required)", html, re.I):
                    raise AccessStopped("Access challenge detected; no bypass/retry attempted")
                self.archive.cache_page(url, 200, body)
                return html
        except HTTPError as error:
            self.archive.cache_page(url, error.code)
            if error.code == 404:
                return None
            raise AccessStopped(f"Stopped on HTTP {error.code}: {url}. No retry, proxy or fallback.") from error
        except (URLError, TimeoutError, OSError, UnicodeDecodeError) as error:
            raise AccessStopped(f"Source unavailable: {url}: {error}. Resume later on a permitted network.") from error

    def check_robots(self):
        html = self._download(BASE + "/robots.txt")
        if not html or "user-agent:" not in html.lower() or "<html" in html.lower():
            raise AccessStopped("Robots policy could not be verified; failing closed")
        self.robots.parse(html.splitlines())
        self.delay = max(self.delay, self.robots.crawl_delay(AGENT))
        self.robots_checked = True

    def get(self, url: str, force=False) -> str | None:
        if not self.robots_checked or not allowed_url(url) or not self.robots.can_fetch(AGENT, url):
            raise AccessStopped(f"Robots/path policy does not permit {url}")
        cached = None if force else self.archive.cached_page(url)
        if cached:
            return cached[1] if cached[0] == 200 else None
        return self._download(url)


@contextmanager
def single_writer(directory: Path):
    directory.mkdir(parents=True, exist_ok=True)
    lock = directory / "collector.lock"
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        raise AccessStopped(f"A collector already owns {lock}. If a process crashed, verify it stopped before removing this lock.") from None
    try:
        with os.fdopen(descriptor, "w") as output:
            output.write(str(os.getpid()))
        yield
    finally:
        lock.unlink(missing_ok=True)


def crawl(archive: Archive, args):
    years = list(range(args.start_season, args.end_season + 1))
    archive.set_meta("requestedSeasons", years)
    archive.set_meta("lastError", "")
    client = Client(archive, args.delay)
    client.check_robots()  # Always refresh access policy, including on a resumed run.
    processed = 0
    for year in years:
        with archive.db:
            archive.db.execute("INSERT OR IGNORE INTO seasons(season) VALUES (?)", (year,))
        try:
            root = f"{BASE}/leagues/NBA_{year}_games.html"
            html = client.get(root, force=args.refresh_schedules)
            if html is None:
                raise ParseError("Season schedule returned 404")
            months, games = schedule(html, year)
            archive.discover(games)
            for month in months:
                monthly_html = client.get(month, force=args.refresh_schedules)
                if monthly_html is None:
                    raise ParseError("Monthly schedule returned 404")
                _, more_games = schedule(monthly_html, year)
                archive.discover(more_games)
            with archive.db:
                archive.db.execute("UPDATE seasons SET schedule_complete=1,error='' WHERE season=?", (year,))
        except ParseError as error:
            with archive.db:
                archive.db.execute("UPDATE seasons SET schedule_complete=0,error=? WHERE season=?", (str(error), year))
            print(f"Season {year}: {error}", file=sys.stderr)
            continue
        for row in archive.db.execute("SELECT * FROM games WHERE season=? AND status!='complete' ORDER BY date,id", (year,)).fetchall():
            if row["status"] in ("missing", "rejected") and not args.retry_missing:
                continue
            if args.max_games and processed >= args.max_games:
                return
            game = {key: row[key] for key in ("id", "season", "date", "home", "away")}
            game["seasonType"] = row["season_type"]
            try:
                box_html = client.get(f"{BASE}/boxscores/{game['id']}.html", force=args.retry_missing)
                pbp_html = client.get(f"{BASE}/boxscores/pbp/{game['id']}.html", force=args.retry_missing)
                if box_html is None or pbp_html is None:
                    archive.mark(game["id"], "missing", "Paired box score or play-by-play returned 404")
                else:
                    events, document = play_by_play(pbp_html, box_score(box_html, game))
                    archive.finish(document, events)
                    print(f"{game['id']}: {len(events)} events", flush=True)
            except ParseError as error:
                archive.mark(game["id"], "rejected", str(error))
                print(f"{game['id']}: rejected: {error}", file=sys.stderr)
            processed += 1


def read_saved(path: Path) -> str:
    if not path.is_file() or path.stat().st_size > MAX_PAGE:
        raise ParseError(f"Missing/oversized saved page: {path.name}")
    return path.read_text(encoding="utf-8")


def ingest(archive: Archive, args):
    root = args.saved_html
    if not root.is_dir():
        raise ValueError("Saved HTML directory does not exist")
    for path in sorted((root / "leagues").glob("NBA_*_games*.html")):
        match = SEASON_PATH.fullmatch("/leagues/" + path.name)
        if not match:
            continue
        _, games = schedule(read_saved(path), int(match[1]))
        archive.discover(games)
        with archive.db:
            archive.db.execute("INSERT OR IGNORE INTO seasons(season,error) VALUES (?,?)", (int(match[1]), "Saved-page discovery; full season coverage not verified"))
    for path in sorted((root / "boxscores").glob("*.html")):
        game_id = path.stem
        if not re.fullmatch(GAME_ID, game_id):
            continue
        existing = archive.db.execute("SELECT * FROM games WHERE id=?", (game_id,)).fetchone()
        if existing and existing["status"] == "complete":
            continue
        try:
            if not existing and not args.season:
                raise ValueError("Provide --season (ending year) when saved schedules do not identify a game")
            game = ({key: existing[key] for key in ("id", "season", "date", "home", "away")} if existing
                    else {"id": game_id, "season": args.season, "date": game_date(game_id)})
            if existing:
                game["seasonType"] = existing["season_type"]
            html = read_saved(path)
            box = box_score(html, game)
            archive.discover({game_id: box})
            pbp = read_saved(root / "boxscores" / "pbp" / path.name)
            archive.cache_page(f"{BASE}/boxscores/{game_id}.html", 200, html.encode())
            archive.cache_page(f"{BASE}/boxscores/pbp/{game_id}.html", 200, pbp.encode())
            events, document = play_by_play(pbp, box)
            archive.finish(document, events)
        except ParseError as error:
            # Keep a discovered game visible as a gap rather than fabricating a replacement.
            archive.mark(game_id, "rejected", str(error))
            print(f"{game_id}: rejected: {error}", file=sys.stderr)
    archive.set_meta("ingestedSavedPages", True)


def possession_estimate(game: dict) -> float:
    return sum(t["fga"] - t["oreb"] + t["turnovers"] + 0.44 * t["fta"] for t in game["totals"].values()) / 2



# ---------------------------------------------------------------------------
# hoopR / SportsDataverse open-dataset ingest (local CSV files, no scraping).
# Files come from the public sportsdataverse-data GitHub releases
# (espn_nba_pbp / espn_nba_player_boxscores). ESPN-sourced data; the user is
# responsible for having the right to use their downloaded copies.
# ---------------------------------------------------------------------------
def _truthy(value) -> bool:
    return str(value).strip().lower() in ("true", "1", "t", "yes")


def _stat(row, key, aliases_map):
    actual = aliases_map.get(key)
    if actual is None:
        return 0
    value = row.get(actual, "")
    try:
        return int(float(str(value).strip() or 0))
    except ValueError:
        return 0


HOOPR_BOX_FIELDS = {
    "points": ["points", "pts"], "threes": ["three_point_field_goals_made", "3pt_field_goals_made", "fg3"],
    "threeAttempts": ["three_point_field_goals_attempted", "3pt_field_goals_attempted", "fg3a"],
    "fgm": ["field_goals_made", "fg"], "fga": ["field_goals_attempted", "fga"],
    "ftm": ["free_throws_made", "ft"], "fta": ["free_throws_attempted", "fta"],
    "oreb": ["offensive_rebounds", "orb"], "rebounds": ["total_rebounds", "rebounds", "trb"],
    "assists": ["assists", "ast"], "steals": ["steals", "stl"], "blocks": ["blocks", "blk"],
    "turnovers": ["turnovers", "tov"], "fouls": ["personal_fouls", "fouls", "pf"],
}
ESPN_TEAM_ALIASES = {"GS": "GSW", "NY": "NYK", "SA": "SAS", "NO": "NOP", "UTAH": "UTA", "WSH": "WAS"}


def _hoopr_team(value: str) -> str:
    value = str(value).strip().upper()
    return "" if value in ("", "NA", "NULL") else canonical_team(ESPN_TEAM_ALIASES.get(value, value))


def _hoopr_minutes(raw: str) -> float:
    try:
        parts = str(raw).strip().split(":")
        minutes = float(parts[0])
        if len(parts) == 2:
            seconds = float(parts[1])
            if not minutes.is_integer() or not math.isfinite(seconds) or not 0 <= seconds < 60:
                return 0.0
            minutes += seconds / 60
        elif len(parts) != 1:
            return 0.0
        return minutes if math.isfinite(minutes) and minutes >= 0 else 0.0
    except ValueError:
        return 0.0


def ingest_hoopr(archive: Archive, args) -> None:
    if not args.player_box_csv:
        raise ParseError("Provide at least one --player-box-csv file from the espn_nba_player_boxscores release")
    games: dict[str, dict] = {}
    order: list[str] = []

    def set_team(game, side, raw):
        team = _hoopr_team(raw)
        if not team:
            return
        if game[side] and game[side] != team:
            game["metadataError"] = "Conflicting home/away team metadata"
        else:
            game[side] = team

    for path in args.player_box_csv:
        with open(path, newline="", encoding="utf-8-sig") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                raise ParseError(f"{path}: empty CSV")
            probe = {"__probe__": 1}
            keys = {str(k).strip().lower(): k for k in reader.fieldnames}
            def resolve(aliases, label, required=True):
                for alias in aliases:
                    if alias in keys:
                        return keys[alias]
                if required:
                    raise ParseError(f"{path}: hoopR CSV missing column for {label}; looked for {aliases}. Available: {sorted(keys)}")
                return None
            cols = {"game": resolve(["game_id"], "game id"), "athlete": resolve(["athlete_id"], "athlete id"),
                    "name": resolve(["athlete_display_name", "display_name", "name"], "athlete name"),
                    "team": resolve(["team_abbreviation", "team_abbr"], "team"),
                    "starter": resolve(["starter"], "starter flag"),
                    "dnp": resolve(["did_not_play"], "did-not-play flag", False),
                    "minutes": resolve(["minutes", "min"], "minutes"),
                    "date": resolve(["game_date", "date"], "game date", False),
                    "home": resolve(["home_team_abbreviation", "home_team_abbr", "home_team_abbrev"], "home team", False),
                    "away": resolve(["away_team_abbreviation", "away_team_abbr", "away_team_abbrev"], "away team", False),
                    "role": resolve(["home_away", "team_home_away"], "home/away role", False),
                    "season_type": resolve(["season_type"], "season type", False)}
            stat_cols = {field_name: resolve(aliases, field_name, False) for field_name, aliases in HOOPR_BOX_FIELDS.items()}
            stat_cols = {k: v for k, v in stat_cols.items() if v}
            for field_name in ("points", "fgm", "fga", "fta", "oreb", "turnovers"):
                if field_name not in stat_cols:
                    raise ParseError(f"{path}: hoopR CSV missing required stat column {field_name}")
            for row in reader:
                game_id = str(row[cols["game"]]).strip()
                if not game_id:
                    continue
                game = games.get(game_id)
                if game is None:
                    game = games[game_id] = {"id": game_id, "season": args.season, "players": [],
                                             "date": "", "home": "", "away": "", "seasonType": "regular"}
                    order.append(game_id)
                if cols["date"] and not game["date"]:
                    game["date"] = str(row[cols["date"]]).strip()[:10]
                if cols["home"]:
                    set_team(game, "home", row[cols["home"]])
                if cols["away"]:
                    set_team(game, "away", row[cols["away"]])
                if cols["season_type"]:
                    game["seasonType"] = "playoffs" if str(row[cols["season_type"]]).strip() in ("3", "playoffs") else "regular"
                team = _hoopr_team(row[cols["team"]])
                role = str(row.get(cols["role"], "")).strip().lower()
                if role in ("home", "away"):
                    set_team(game, role, team)
                starter = _truthy(row[cols["starter"]])
                minutes_value = _hoopr_minutes(row[cols["minutes"]])
                if (cols["dnp"] and _truthy(row[cols["dnp"]])) or (minutes_value <= 0 and not starter):
                    continue
                if starter and minutes_value <= 0:
                    raise ParseError(f"{game_id}: hoopR starter with no minutes; refusing to guess the lineup")
                log = {"date": game["date"] or "0000-00-00", "gameId": game_id,
                       "home": team == (game["home"] or team), "minutes": minutes_value}
                for field_name in HOOPR_BOX_FIELDS:
                    log[field_name] = _stat(row, field_name, stat_cols)
                player_id = "espn-" + str(row[cols["athlete"]]).strip()
                game["players"].append({"id": player_id, "name": str(row[cols["name"]]).strip(),
                                        "team": team, "starter": starter, "log": log})
    # Play-by-play (optional but required for verified first-basket features).
    pbp: dict[str, list[dict]] = defaultdict(list)
    first_fg: dict[str, dict] = {}
    for path in args.pbp_csv or []:
        with open(path, newline="", encoding="utf-8-sig") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                raise ParseError(f"{path}: empty CSV")
            keys = {str(k).strip().lower(): k for k in reader.fieldnames}
            def resolve(aliases, label, required=True):
                for alias in aliases:
                    if alias in keys:
                        return keys[alias]
                if required:
                    raise ParseError(f"{path}: hoopR PBP CSV missing column for {label}; looked for {aliases}. Available: {sorted(keys)}")
                return None
            cols = {"game": resolve(["game_id"], "game id"),
                    "text": resolve(["text", "play_text", "description"], "play text"),
                    "type": resolve(["type_text"], "play type", False),
                    "period": resolve(["period_number", "period"], "period"),
                    "clock": resolve(["clock_display_value", "clock"], "clock", False),
                    "scoring": resolve(["scoring_play"], "scoring flag", False),
                    "value": resolve(["score_value"], "score value", False),
                    "away_score": resolve(["away_score"], "away score", False),
                    "home_score": resolve(["home_score"], "home score", False),
                    "athlete": resolve(["athlete_id_1"], "primary athlete", False),
                    "home": resolve(["home_team_abbreviation", "home_team_abbr", "home_team_abbrev"], "home team", False),
                    "away": resolve(["away_team_abbreviation", "away_team_abbr", "away_team_abbrev"], "away team", False)}
            for row in reader:
                game_id = str(row[cols["game"]]).strip()
                game = games.get(game_id)
                if game is None:
                    continue
                if cols["home"]:
                    set_team(game, "home", row[cols["home"]])
                if cols["away"]:
                    set_team(game, "away", row[cols["away"]])
                try:
                    period = int(float(str(row[cols["period"]]).strip() or 1))
                except ValueError:
                    continue
                scoring = _truthy(row[cols["scoring"]]) if cols["scoring"] else False
                try:
                    value = int(float(str(row[cols["value"]]).strip() or 0)) if cols["value"] else 0
                except ValueError:
                    value = 0
                athlete = str(row[cols["athlete"]]).strip() if cols["athlete"] else ""
                event = {"sequence": len(pbp[game_id]) + 1, "period": period,
                         "clock": str(row[cols["clock"]]).strip() if cols["clock"] else "",
                         "description": str(row[cols["text"]]).strip(),
                         "type": ("made_free_throw" if value == 1 else "made_field_goal") if scoring and value else "other",
                         "playerId": ("espn-" + athlete) if athlete else None,
                         "awayScore": _stat(row, "x", {"x": cols["away_score"]}) if cols["away_score"] else None,
                         "homeScore": _stat(row, "x", {"x": cols["home_score"]}) if cols["home_score"] else None}
                pbp[game_id].append(event)
                if scoring and value >= 2 and game_id not in first_fg and event["playerId"]:
                    first_fg[game_id] = {"playerId": event["playerId"], "period": period,
                                         "clock": event["clock"], "description": event["description"]}
    archive.set_meta("sources", "sportsdataverse hoopR ESPN play-by-play + player box scores (local CSV ingest)")
    discovered, completed, rejected = {}, 0, 0
    for game_id in order:
        game = games[game_id]
        try:
            if game.get("metadataError"):
                raise ParseError(game["metadataError"])
            teams = {p["team"] for p in game["players"]}
            if len(teams) != 2 or not game["date"] or not game["home"] or not game["away"]:
                raise ParseError("Missing date or home/away teams; provide schedule columns in the CSVs")
            if game["home"] not in teams or game["away"] not in teams or game["home"] == game["away"]:
                raise ParseError("Home/away columns disagree with player team columns")
            for player in game["players"]:
                player["log"]["date"] = game["date"]
                player["log"]["home"] = player["team"] == game["home"]
            totals = {}
            for team in (game["home"], game["away"]):
                roster = [p for p in game["players"] if p["team"] == team]
                if sum(p["starter"] for p in roster) != 5:
                    raise ParseError("Historical starting lineup is not exactly five per team")
                totals[team] = {field_name: sum(p["log"][field_name] for p in roster) for field_name in HOOPR_BOX_FIELDS}
            events = pbp.get(game_id, [])
            maximum_period = max((e["period"] for e in events), default=4)
            scorer_ids = {p["id"] for p in game["players"]}
            fg = first_fg.get(game_id)
            monotone = all((events[i]["awayScore"] or 0) >= (events[i - 1]["awayScore"] or 0) and
                           (events[i]["homeScore"] or 0) >= (events[i - 1]["homeScore"] or 0)
                           for i in range(1, len(events))) if events else False
            document = {"id": game_id, "season": args.season, "date": game["date"], "home": game["home"],
                        "away": game["away"], "seasonType": game["seasonType"], "players": game["players"],
                        "totals": totals,
                        "durationMinutes": 48 + max(0, maximum_period - 4) * 5,
                        "firstFieldGoal": fg if (fg and fg["playerId"] in scorer_ids and monotone) else None,
                        "firstFieldGoalVerified": bool(fg and fg["playerId"] in scorer_ids and monotone),
                        "sourceUrl": "https://github.com/sportsdataverse/sportsdataverse-data/releases"}
            discovered[game_id] = document
            archive.discover({game_id: document})
            archive.finish_external(document, events)
            completed += 1
        except ParseError as error:
            rejected += 1
            print(f"hoopR {game_id}: rejected: {error}", file=sys.stderr)
    if not completed:
        raise ParseError("No hoopR games could be archived; see rejections above")
    print(f"hoopR ingest: {completed} games archived, {rejected} rejected, {sum(len(v) for v in pbp.values())} PBP events")


def export_features(archive: Archive, as_of: date, recent_days=210):
    """All events stay archived; export only model-sized, strictly pre-cutoff features."""
    cutoff = as_of.isoformat()
    recent = (as_of - timedelta(days=recent_days)).isoformat()
    active = {}
    for row in archive.db.execute("SELECT document FROM games WHERE status='complete' AND date>=? AND date<? ORDER BY date,id", (recent, cutoff)):
        game = json.loads(row[0])
        for player in game["players"]:
            active[player["id"]] = {"name": player["name"], "team": player["team"], "latestDate": game["date"]}
    if len(active) > 1200:
        raise ValueError("More than 1200 recent players; reduce --recent-days. The raw archive is unchanged.")
    crosswalk = {}
    for player_id, player in active.items():
        key = normalized_name(player["name"])
        if not key or key in crosswalk:
            raise ValueError("Ambiguous player-name crosswalk; refusing to merge different historical IDs")
        crosswalk[key] = player_id
    logs = defaultdict(lambda: deque(maxlen=150))
    first_baskets = defaultdict(lambda: deque(maxlen=2000))
    teams = defaultdict(lambda: deque(maxlen=150))
    historical_paces = defaultdict(lambda: deque(maxlen=150))
    for row in archive.db.execute("SELECT document FROM games WHERE status='complete' AND date<? ORDER BY date,id", (cutoff,)):
        game = json.loads(row[0])
        possessions = possession_estimate(game)
        pace = possessions * 48 / game["durationMinutes"]
        for player in game["players"]:
            player_id = player["id"]
            if player_id not in active:
                continue
            if game["seasonType"] == "regular" and player["log"]["minutes"] >= 0.1:
                logs[player_id].append(player["log"])
                historical_paces[player_id].append(pace)
            if (player["starter"] and game["firstFieldGoalVerified"]
                    and game["seasonType"] in ("regular", "playoffs")):
                first_baskets[player_id].append({"gameId": game["id"], "date": game["date"],
                                                 "win": game["firstFieldGoal"]["playerId"] == player_id})
        if game["seasonType"] == "regular":
            for team, stats in game["totals"].items():
                opponent = game["away"] if team == game["home"] else game["home"]
                points, against = stats["points"], game["totals"][opponent]["points"]
                teams[team].append({"date": game["date"], "gameId": game["id"], "points": points,
                                    "margin": points - against, "home": team == game["home"],
                                    "opponentPoints": against, "possessions": possessions,
                                    "durationMinutes": game["durationMinutes"]})
    records = []
    for team, sample in sorted(teams.items()):
        if not sample or sample[-1]["date"] < recent:
            continue
        possessions = sum(g["possessions"] for g in sample)
        if possessions <= 0:
            continue
        pace = sum(g["possessions"] * 48 / g["durationMinutes"] for g in sample) / len(sample)
        offense = sum(g["points"] for g in sample) * 100 / possessions
        defense = sum(g["opponentPoints"] for g in sample) * 100 / possessions
        if not (70 <= pace <= 130 and 70 <= offense <= 150 and 70 <= defense <= 150):
            continue  # Do not silently clamp incomplete box-score environments into plausible inputs.
        feature = {"abbr": team, "pace": pace, "offRating": offense, "defRating": defense,
                   "logs": list(reversed(sample)), "dataAsOf": sample[-1]["date"] + "T23:59:59Z"}
        records.append({"recordType": "team", "feature": feature})
    for player_id, player in sorted(active.items()):
        sample = list(reversed(logs[player_id]))
        observations = list(reversed(first_baskets[player_id]))
        if not sample and not observations:
            continue
        feature = {"name": player["name"], "team": player["team"], "basketballReferenceId": player_id,
                   "logs": sample, "firstBasketHistory": observations, "starterConfirmed": False,
                   "injury": "Unknown", "dataAsOf": player["latestDate"] + "T23:59:59Z"}
        if historical_paces[player_id]:
            pace = sum(historical_paces[player_id]) / len(historical_paces[player_id])
            if 70 <= pace <= 130:
                feature["historicalPace"] = pace
        records.append({"recordType": "player", "feature": feature})
    metadata = {**archive.status(), "asOf": cutoff, "exportedAt": utc_now(),
                "recentDays": recent_days, "players": sum(r["recordType"] == "player" for r in records),
                "teams": sum(r["recordType"] == "team" for r in records)}
    manifest = {"recordType": "manifest", "schemaVersion": 1,
                "source": archive.meta("sources") or "basketball_reference", "metadata": metadata}
    target = archive.directory / "features.jsonl"
    temp = target.with_suffix(".tmp")
    with temp.open("w", encoding="utf-8", newline="\n") as output:
        for record in (manifest, *records):
            line = json.dumps(record, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            if len(line.encode("utf-8")) > 1024 * 1024:
                raise ValueError("Historical feature record exceeds app limit; previous bundle retained")
            output.write(line + "\n")
    os.replace(temp, target)
    return metadata


def permission(parser):
    parser.add_argument("--permission-confirmed", action="store_true",
                        help="Confirm permission for this collection/use under Sports Reference terms; not permission granted by Edgecourt")


def main(argv=None):
    today = datetime.now(timezone.utc).date()
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("crawl", "ingest", "hoopr", "export", "status"):
        sub = commands.add_parser(name)
        sub.add_argument("--output", type=Path, default=Path("edgecourt-data/history"), help="Local archive directory (keep outside Git)")
        if name != "status":
            sub.add_argument("--as-of", type=date.fromisoformat, default=today, help="Exclusive feature cutoff YYYY-MM-DD; never includes the target day")
            sub.add_argument("--recent-days", type=int, default=210, help="Select recently active players; archive retains all collected seasons")
        if name in ("crawl", "ingest", "hoopr"):
            permission(sub)
        if name == "crawl":
            sub.add_argument("--start-season", type=int, default=1997, help="Season ending year; default 1996-97 play-by-play era")
            sub.add_argument("--end-season", type=int, default=today.year + (today.month >= 10))
            sub.add_argument("--delay", type=float, default=3.1, help="Request interval in seconds; minimum 3.1, increased by robots policy")
            sub.add_argument("--max-games", type=int, default=0, help="Optional per-run game limit; zero means all discovered games")
            sub.add_argument("--refresh-schedules", action="store_true", help="Re-discover schedules, including new games in an ongoing season")
            sub.add_argument("--retry-missing", action="store_true", help="Retry missing/rejected pages on a later authorized run; not access denials")
        if name == "ingest":
            sub.add_argument("--saved-html", type=Path, required=True, help="Root with leagues/ and paired boxscores/pbp/ saved pages")
            sub.add_argument("--season", type=int, help="Season ending year if saved schedules are absent")
        if name == "hoopr":
            sub.add_argument("--pbp-csv", type=Path, action="append", default=[], help="play_by_play_{season}.csv from the sportsdataverse-data espn_nba_pbp release")
            sub.add_argument("--player-box-csv", type=Path, action="append", default=[], help="player_box_{season}.csv from the espn_nba_player_boxscores release")
            sub.add_argument("--season", type=int, required=True, help="Season ending year of the CSV files (ESPN play-by-play era 2002+)")
    args = parser.parse_args(argv)
    if args.command in ("crawl", "ingest", "hoopr") and not args.permission_confirmed:
        parser.error("No collection performed. Confirm that you have permission with --permission-confirmed. Read https://www.sports-reference.com/data_use.html first.")
    if args.command != "status" and not 1 <= args.recent_days <= 365:
        parser.error("--recent-days must be 1..365")
    if args.command == "crawl" and (not 1997 <= args.start_season <= args.end_season <= today.year + 1 or args.max_games < 0 or not math.isfinite(args.delay) or args.delay < 0):
        parser.error("Use supported season ending years 1997..current season, a nonnegative limit and delay")
    if args.command == "ingest" and args.season and not 1997 <= args.season <= today.year + 1:
        parser.error("--season must be a supported season ending year")
    if args.command == "hoopr" and not 2002 <= args.season <= today.year + 1:
        parser.error("hoopR ESPN play-by-play covers season ending years 2002..current season")
    if args.command != "status" and args.as_of > today:
        parser.error("--as-of cannot be in the future")
    try:
        with single_writer(args.output):
            archive = Archive(args.output)
            try:
                if args.command == "status":
                    print(json.dumps(archive.status(), indent=2))
                    return 0
                try:
                    if args.command == "crawl":
                        crawl(archive, args)
                    elif args.command == "ingest":
                        ingest(archive, args)
                    elif args.command == "hoopr":
                        ingest_hoopr(archive, args)
                except (AccessStopped, KeyboardInterrupt) as error:
                    archive.set_meta("lastError", str(error) or "Collection interrupted; resume from cached pages")
                    export_features(archive, args.as_of, args.recent_days)
                    raise
                metadata = export_features(archive, args.as_of, args.recent_days)
                print(json.dumps(metadata, indent=2))
                print(f"Feature bundle: {args.output / 'features.jsonl'}", file=sys.stderr)
                return 0
            finally:
                archive.db.close()
    except (AccessStopped, ParseError, OSError, ValueError, sqlite3.Error) as error:
        print(f"History: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("History: interrupted; completed games are preserved. Resume the same command.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
