#!/usr/bin/env python3
"""Image regression: live stdin, UTF-8 fragments, shutdown, crash and restart."""
import argparse, json, os, sys, time
from pathlib import Path
parser = argparse.ArgumentParser(description='Live-stdin regression using the unchanged official Session')
parser.add_argument('--image', required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'protocol_check'))
from _transport import Session
cfg = json.loads((root / 'protocol_check/config.json').read_text())
s = Session(cfg, args.output / 'session.stderr.log', image=args.image)
events = []
def request(sql=None, ok=True, rows=None, **kw):
    before = time.monotonic()
    response = s.request(sql=sql, **kw)
    assert response['ok'] is ok, response
    if rows is not None:
        assert response.get('rows') == rows, response
    assert not s.proc.stdin.closed
    events.append({'request': sql or kw.get('op'), 'response': response,
                   'seconds': time.monotonic()-before, 'stdin_open': True})
def shutdown_open():
    request(op='shutdown')
    s.proc.wait(timeout=15)
    assert not s.proc.stdin.closed
    assert s.proc.returncode == 0 and not s.buffer and s.proc.stdout.read() == b''
    events.append({'exit_before_stdin_close': True})
    s.stop()
try:
    s.start()
    request('CREATE TABLE probe (id BIGINT PRIMARY KEY, value VARCHAR(32));')
    request("INSERT INTO probe VALUES (1,'first;line');")
    request("INSERT INTO probe VALUES (1,'duplicate');", ok=False)
    request('SELECT id,value FROM probe ORDER BY id;', rows=[[1,'first;line']])
    request(op='unsupported_operation', ok=False)
    request('SELECT id FROM probe;', mode='invalid_mode', ok=False)
    # A valid request split inside a multibyte UTF-8 character must
    # wait for its newline, then return while stdin remains open.
    raw = (json.dumps({'id':'split-utf8','op':'execute',
                       'sql':"SELECT id FROM probe WHERE value='中';",
                       'search_mode':'exact'}, ensure_ascii=False)+'\n').encode()
    cut = raw.index('中'.encode()) + 1
    assert os.write(s.proc.stdin.fileno(), raw[:cut]) == cut
    assert not s.buffer and not s.selector.select(0.05)
    assert os.write(s.proc.stdin.fileno(), raw[cut:]) == len(raw)-cut
    response = s.read(cfg['request_timeout'])
    assert response['id'] == 'split-utf8' and response['ok'] is True and response['rows'] == []
    assert not s.proc.stdin.closed
    events.append({'fragmented_utf8_request':True, 'response':response, 'stdin_open':True})
    request('BEGIN;')
    request("INSERT INTO probe VALUES (2,'rollback');")
    shutdown_open()
    s.start()
    request('SELECT id FROM probe ORDER BY id;', rows=[[1]])
    request('BEGIN;')
    request("INSERT INTO probe VALUES (3,'committed');")
    request('COMMIT;')
    request('BEGIN;')
    request("INSERT INTO probe VALUES (4,'uncommitted');")
    s.crash()
    s.start()
    request('SELECT id FROM probe ORDER BY id;', rows=[[1],[3]])
    shutdown_open()
finally:
    try:
        s.close()
    finally:
        (args.output / 'events.json').write_text(json.dumps(events, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print('PASS: official Session, live stdin, success/error continuation, shutdown before EOF, rollback, crash and restart')
