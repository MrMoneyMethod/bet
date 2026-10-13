#!/usr/bin/env python3
"""Integration tests against a real native C++ process. No external feed calls."""
import csv
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

binary = str(Path(sys.argv[1]).resolve())
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
base = f'http://127.0.0.1:{port}'

def request(path, data=None, token=None, headers=None):
    headers = dict(headers or {})
    if data is not None:
        headers['Content-Type'] = 'application/json'
    if token is not None:
        headers['X-Edgecourt-Token'] = token
    req = Request(base + path, data=None if data is None else json.dumps(data).encode(), headers=headers)
    try:
        with urlopen(req, timeout=10) as response:
            body = response.read()
            return response.status, body, response.headers
    except HTTPError as error:
        return error.code, error.read(), error.headers

with tempfile.TemporaryDirectory(prefix='edgecourt-api-') as directory:
    process = subprocess.Popen([binary, '--demo', '--no-browser', '--port', str(port), '--data-dir', directory], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                status, body, _ = request('/api/health')
                if status == 200:
                    break
            except (URLError, TimeoutError):
                if process.poll() is not None:
                    raise RuntimeError(process.stdout.read().decode(errors='replace'))
            if time.monotonic() > deadline:
                raise RuntimeError('C++ server did not start in time')
            time.sleep(0.1)
        assert json.loads(body)['app'] == 'Edgecourt'
        status, body, headers = request('/')
        assert status == 200 and b'app.js' in body
        assert "default-src 'self'" in headers['Content-Security-Policy']
        assert not headers.get('X-Frame-Options'), 'Arena preview must be embeddable'
        for path, expected in [('/app.js', b'filteredMarkets'), ('/styles.css', b'.topbar'), ('/favicon.svg', b'<svg')]:
            status, body, _ = request(path)
            assert status == 200 and expected in body
        _, body, _ = request('/api/state')
        state = json.loads(body)
        token = state['authToken']
        assert len(token) == 64 and state['mode'] == 'demo'
        assert len(state['featureCatalog']) == 38
        assert state['historicalData']['state'] == 'not_installed'
        assert state['historicalData']['games'] == 0
        assert all(m.get('oddsPercent') is not None for m in state['markets'] if m.get('americanOdds') is not None)
        assert request('/api/history/reload', {})[0] == 403
        assert request('/api/history/reload', {}, token)[0] == 200
        assert state['summary']['quotes'] > 100
        assert all(m.get('demo', False) for m in state['markets'] if m.get('americanOdds') is not None)
        assert request('/api/refresh', {})[0] == 403
        assert request('/api/refresh', {}, token, {'Origin': 'https://evil.example'})[0] == 403
        assert request('/api/state', headers={'Host': 'attacker.example'})[0] == 403
        assert request('/api/health', headers={'Host': f'{port}-test.e2b.app'})[0] == 200
        assert request('/api/mode', {'mode': 'not-a-mode'}, token)[0] == 400
        game = next(g for g in state['games'] if g['home'] == 'LAL')
        observed = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        quote = dict(gameId=game['id'], market='points', player='Stephen Curry', team='GSW', side='over', line=28.5, americanOdds=120, book='Smoke test', observedAt=observed)
        status, result, _ = request('/api/import', {'format':'json','data':json.dumps({'quotes':[quote]})}, token)
        assert status == 200, result
        assert not json.loads(result)['persisted'], 'Demo imports must not persist into real mode'
        _, body, _ = request('/api/state')
        after = json.loads(body)
        assert after['summary']['quotes'] == state['summary']['quotes'] + 1
        imported = next(m for m in after['markets'] if m['book'] == 'Smoke test')
        assert imported['source'] == 'user_import' and imported['ev'] is not None
        assert request('/api/import', {'format':'json','data':json.dumps({'quotes':[quote]})}, token)[0] == 200
        _, body, _ = request('/api/state')
        assert json.loads(body)['summary']['quotes'] == after['summary']['quotes'], 'Imports must deduplicate'
        bad = dict(quote, americanOdds=0)
        assert request('/api/import', {'format':'json','data':json.dumps({'quotes':[quote,bad]})}, token)[0] == 400
        _, body, _ = request('/api/state')
        assert json.loads(body)['summary']['quotes'] == after['summary']['quotes'], 'Bad batch must be atomic'
        orphan = dict(quote, gameId='unknown-event')
        assert request('/api/import', {'format':'json','data':json.dumps({'quotes':[orphan]})}, token)[0] == 400
        # Quoted commas in bookmaker names must survive CSV import.
        stream = io.StringIO(newline='')
        writer = csv.DictWriter(stream, fieldnames=list(quote))
        writer.writeheader()
        writer.writerow(dict(quote, book='CSV, "Book"'))
        assert request('/api/import', {'format':'csv','data':stream.getvalue()}, token)[0] == 200
        status, body, headers = request('/api/template.csv')
        assert status == 200 and b'gameId,market' in body and 'attachment' in headers['Content-Disposition']
        assert request('/api/refresh', {}, token)[0] == 200
        _, body, _ = request('/api/state')
        assert json.loads(body)['summary']['quotes'] == state['summary']['quotes'], 'Demo refresh resets temporary imports'
        assert not (Path(directory) / 'imports.json').exists(), 'Demo-only imports should not be saved'
        percent_quote = dict(quote, book='Percentage smoke', oddsPercent=52.1234567)
        del percent_quote['americanOdds']
        assert request('/api/import', {'format':'json','data':json.dumps({'quotes':[percent_quote]})}, token)[0] == 200
        _, body, _ = request('/api/state')
        percent_row = next(m for m in json.loads(body)['markets'] if m['book'] == 'Percentage smoke')
        assert percent_row['americanOdds'] is None and abs(percent_row['oddsPercent']-52.1234567) < 1e-9
        assert percent_row['ev'] is not None and abs(percent_row['ev']-100*(percent_row['probability']/.521234567+percent_row['pushProbability']-1)) < 1e-7
        bad_percent = dict(percent_quote, oddsPercent=100)
        assert request('/api/import', {'format':'json','data':json.dumps({'quotes':[bad_percent]})}, token)[0] == 400
        assert request('/api/import', {'format':'json','data':json.dumps({'lineups':[{'gameId':'orphan','player':'Stephen Curry','team':'GSW','confirmed':True}]})}, token)[0] == 400
        assert request('/unrecognized-route')[0] == 404
        print('PASS API: native server, embedded assets, CSRF/Origin/Host guards, quote imports, deduplication, atomic validation, CSV/template, synthetic isolation and refresh')
    finally:
        process.terminate()
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        output = process.stdout.read().decode(errors='replace')
        if process.returncode not in (0, -15, 1) and os.name != 'nt':
            print(output, file=sys.stderr)
