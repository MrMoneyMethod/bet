#!/usr/bin/env python3
"""Synthetic saved HTML -> archive -> streaming features -> native model, without network."""
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from history_fixtures import game_pages

binary = str(Path(sys.argv[1]).resolve())
root = Path(__file__).resolve().parents[1]
with socket.socket() as socket_:
    socket_.bind(('127.0.0.1', 0))
    port = socket_.getsockname()[1]
base = f'http://127.0.0.1:{port}'


def request(path, data=None, token=None):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['X-Edgecourt-Token'] = token
    req = Request(base + path, headers=headers, data=json.dumps(data).encode() if data is not None else None)
    try:
        with urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except HTTPError as error:
        return error.code, json.load(error)


with tempfile.TemporaryDirectory(prefix='edgecourt-history-native-') as directory:
    directory = Path(directory)
    raw = directory / 'saved'
    (raw / 'boxscores' / 'pbp').mkdir(parents=True)
    today = datetime.now(timezone.utc)
    for days in range(1, 21):
        game_id = (today - timedelta(days=days)).strftime('%Y%m%d') + '0BOS'
        _, box, pbp, _ = game_pages(game_id)
        (raw / 'boxscores' / (game_id + '.html')).write_text(box, encoding='utf-8')
        (raw / 'boxscores' / 'pbp' / (game_id + '.html')).write_text(pbp, encoding='utf-8')
    data_dir = directory / 'workspace'
    history_dir = data_dir / 'history'
    # Permission is for our own generated fixtures, not provider data. This command performs no networking.
    subprocess.run([sys.executable, str(root / 'scripts' / 'basketball_reference_history.py'), 'ingest',
                    '--saved-html', str(raw), '--season', str(today.year), '--output', str(history_dir),
                    '--permission-confirmed'], check=True, capture_output=True)
    process = subprocess.Popen([binary, '--live', '--offline', '--no-browser', '--port', str(port),
                                '--data-dir', str(data_dir)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                status, state = request('/api/state')
                if status == 200:
                    break
            except (URLError, TimeoutError):
                if process.poll() is not None:
                    raise RuntimeError(process.stdout.read().decode(errors='replace'))
            if time.monotonic() > deadline:
                raise RuntimeError('Native history workspace did not start')
            time.sleep(.1)
        assert state['mode'] == 'live' and state['publicFeedsEnabled'] is False
        assert not state['refreshing']
        assert state['historicalData']['games'] == 20 and state['historicalData']['players'] == 10
        assert state['summary']['players'] == 10
        assert state['summary']['quotes'] == 0 and state['summary']['positiveEv'] == 0
        token = state['authToken']
        target = {'id':'history-target','home':'BOS','away':'ATL','status':'upcoming','seasonType':'regular',
                  'startsAt':(today+timedelta(hours=24)).strftime('%Y-%m-%dT%H:%M:%SZ')}
        quote = {'gameId':target['id'],'market':'points','player':'Fixture Away 1','team':'ATL','side':'over',
                 'line':18.5,'oddsPercent':52.38,'book':'Synthetic fixture book','observedAt':today.strftime('%Y-%m-%dT%H:%M:%SZ')}
        first = dict(quote, market='first_basket', side='yes', line=None, oddsPercent=10)
        status, result = request('/api/import', {'format':'json','data':json.dumps({'games':[target],'quotes':[quote,first]})}, token)
        assert status == 200, result
        _, state = request('/api/state')
        points = next(m for m in state['markets'] if m['market']=='points' and m['book']==quote['book'])
        first_row = next(m for m in state['markets'] if m['market']=='first_basket' and m['book']==quote['book'])
        assert points['probability'] is not None and points['sampleSize']==20
        assert points['historySource']=='basketball_reference'
        assert abs(points['ev']-100*(points['probability']/.5238+points['pushProbability']-1)) < 1e-7
        assert first_row['probability'] is None, 'Historical starters must never confirm the current game'
        lineups = [{'gameId':target['id'],'player':f'Fixture {side.title()} {i}',
                    'team':'ATL' if side=='away' else 'BOS','confirmed':True}
                   for side in ('away','home') for i in range(1,6)]
        status, result = request('/api/import', {'format':'json','data':json.dumps({'lineups':lineups})}, token)
        assert status == 200, result
        assert result['lineups']==10 and result['players']==0
        _, state = request('/api/state')
        first_row = next(m for m in state['markets'] if m['market']=='first_basket' and m['book']==quote['book'])
        assert abs(first_row['probability']-.82) < 1e-8
        assert first_row['sampleSize']==20
        assert any(f['label']=='First-basket wins' and f['value']==20 for f in first_row['features'])
        assert request('/api/refresh', {}, token)[0] == 200
        _, state = request('/api/state')
        assert not state['refreshing'] and state['historicalData']['games']==20
        feature_path = history_dir / 'features.jsonl'
        good = feature_path.read_text(encoding='utf-8')
        feature_path.write_text(good + '{corrupt}\n', encoding='utf-8')
        status, result = request('/api/history/reload', {}, token)
        assert status==400 and 'previous bundle retained' in result['error']
        _, state = request('/api/state')
        assert state['historicalData']['games']==20
        feature_path.write_text(good, encoding='utf-8')
        assert request('/api/history/reload', {}, token)[0]==200
        # Removing a bundle must remove its features, rather than leave stale data in the model.
        feature_path.unlink()
        assert request('/api/history/reload', {}, token)[0]==200
        _, state = request('/api/state')
        assert state['historicalData']['state']=='not_installed' and state['summary']['players']==0
        points = next(m for m in state['markets'] if m['market']=='points' and m['book']==quote['book'])
        assert points['probability'] is None and points['oddsPercent']==52.38
        assert request('/api/mode', {'mode':'demo'}, token)[0]==200
        _, state = request('/api/state')
        assert state['mode']=='demo' and all(m.get('demo',False) for m in state['markets'] if m.get('oddsPercent') is not None)
        print('PASS historical pipeline: synthetic saved HTML, complete event archive, streaming features, exact percentage EV, dated first-FG probabilities, separate game-scoped lineups, offline isolation, atomic reload and removed-bundle cleanup')
    finally:
        process.terminate()
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
