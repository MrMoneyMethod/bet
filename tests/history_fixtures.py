"""Entirely synthetic HTML fixtures. No Basketball Reference data is redistributed."""
from datetime import datetime
from html import escape


def game_pages(game_id='202604010BOS', season=2026, overtime=False, comments=False):
    away, home = 'ATL', game_id[-3:]
    groups = {}
    tables = []
    columns = 'mp fg fga fg3 fg3a ft fta orb trb ast stl blk tov pf pts'.split()
    for side, team in [('away', away), ('home', home)]:
        players = []
        rows = []
        for index in range(5):
            identity = f'{side}{index+1:04}'
            name = f'Fixture {side.title()} {index+1}'
            fg = 12 if index == 0 else 10
            ft = int(side == 'home' and index == 1)
            stats = dict(mp='53:00' if overtime else '48:00', fg=fg, fga=18, fg3=0, fg3a=2,
                         ft=ft, fta=2, orb=2, trb=4, ast=2, stl=1, blk=0, tov=2, pf=1, pts=fg*2+ft)
            link = f'<a href="/players/{side[0]}/{identity}.html">{name}</a>'
            rows.append(f'<tr><th data-stat="player">{link}</th>' + ''.join(f'<td data-stat="{key}">{stats[key]}</td>' for key in columns) + '</tr>')
            players.append(dict(id=identity, name=name, link=link, stats=stats))
        totals = {key: sum(p['stats'][key] for p in players) for key in columns if key != 'mp'}
        rows.append('<tr class="thead"><th data-stat="player">Reserves</th></tr>')
        rows.append(f'<tr><th data-stat="player"><a href="/players/d/dnp0001.html">Fixture DNP {side}</a></th><td data-stat="reason" colspan="15">Inactive</td></tr>')
        rows.append('<tr><th data-stat="player">Team Totals</th>' + ''.join(f'<td data-stat="{key}">{totals.get(key, "265:00" if overtime else "240:00")}</td>' for key in columns) + '</tr>')
        table = f'<table id="box-{team}-game-basic"><thead><tr><th data-stat="player">Starters</th></tr></thead><tbody>' + ''.join(rows) + '</tbody></table>'
        tables.append(f'<!--{table}-->' if comments else table)
        groups[side] = players
    box = '<html><head><title>Fixture NBA regular game</title></head><body>' + ''.join(tables) + '</body></html>'
    away_score, home_score = 0, 0
    rows = ['<tr><th colspan="6">1st Q</th></tr>', '<tr><td>12:00.0</td><td colspan="5">Start of 1st quarter</td></tr>']

    def event(side, player, description, clock='11:58.0', points=2):
        nonlocal away_score, home_score
        if side == 'away':
            away_score += points
        else:
            home_score += points
        cells = [clock, description if side == 'away' else '', f'+{points}' if side == 'away' else '',
                 f'{away_score}-{home_score}', f'+{points}' if side == 'home' else '', description if side == 'home' else '']
        rows.append('<tr>' + ''.join(f'<td>{cell}</td>' for cell in cells) + '</tr>')

    # First point is deliberately a free throw. First made FIELD GOAL is a different player/team.
    player = groups['home'][1]
    event('home', player, player['link'] + ' makes free throw 1 of 2', clock='11:59.0', points=1)
    for side in ('away', 'home'):
        for player in groups[side]:
            for _ in range(player['stats']['fg']):
                assist = groups[side][2]['link']
                event(side, player, player['link'] + ' makes 2-pt jump shot (assist by ' + assist + ')')
    rows.append('<tr><td>0:00.0</td><td colspan="5">End of 1st quarter</td></tr>')
    for period, ordinal in [(2, '2nd'), (3, '3rd'), (4, '4th')]:
        rows.extend([f'<tr><th colspan="6">{ordinal} Q</th></tr>',
                     f'<tr><td>12:00.0</td><td colspan="5">Start of {ordinal} quarter</td></tr>',
                     f'<tr><td>0:00.0</td><td colspan="5">End of {ordinal} quarter</td></tr>'])
    if overtime:
        rows.extend(['<tr><th colspan="6">1st OT</th></tr>',
                     '<tr><td>5:00.0</td><td colspan="5">Start of 1st overtime</td></tr>',
                     '<tr><td>0:00.0</td><td colspan="5">End of 1st overtime</td></tr>'])
    pbp = '<html><table id="pbp"><tbody>' + ''.join(rows) + '</tbody></table></html>'
    date = datetime.strptime(game_id[:8], '%Y%m%d').date().isoformat()
    schedule = f'''<html><a href="/leagues/NBA_{season}_games-april.html">April</a>
    <table id="schedule"><tbody><tr><th data-stat="date_game">{escape(date)}</th>
    <td data-stat="visitor_team_name"><a href="/teams/{away}/{season}.html">Fixture away</a></td>
    <td data-stat="home_team_name"><a href="/teams/{home}/{season}.html">Fixture home</a></td>
    <td data-stat="box_score_text"><a href="/boxscores/{game_id}.html">Box Score</a></td></tr></tbody></table></html>'''
    game = dict(id=game_id, season=season, date=date, home=home, away=away, seasonType="regular")
    return game, box, pbp, schedule
