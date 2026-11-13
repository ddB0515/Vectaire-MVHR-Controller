#!/usr/bin/env python3
"""Preview the controller's web page on this computer, without building or flashing.

Serves components/web/www/*.html straight from disk (edit a file, then refresh the browser) and answers the
page's /api/* calls with made-up data shaped like web.c's state_get(). Python 3 standard library only.

Usage:  python tools/web_preview.py [port]        then open  http://localhost:8080/

A "Preview controls" panel (bottom right of the page, from tools/web_preview_panel.html) changes the mock data
with sliders and menus. The same can be done by opening URLs like:
  /mock?function=Humidity%20boost&boost=On      set any field of /api/state (values are read as JSON, else text)
  /mock?alarms=1&alarm=Filter%20change          show an alarm
  /mock?online=false                            unit not responding
  /mock?reset=1                                 back to the defaults
Controls on the page (fan, bypass, holiday, night, setpoints) change the mock state too, so you can click through.
Any password is accepted.
"""
import copy
import json
import re
import os
import sys
import time
import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WWW = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'components', 'web', 'www')
PANEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web_preview_panel.html')
START = time.time()

DEFAULT = {
    'online': True, 'simulated': True,
    't_in': 22.9, 't_out': 12.2, 'rh': 40.0,
    'fan_a': 86, 'fan_b': 90, 'fan': 2,
    'bypass': 'auto', 'bypass_state': 0, 'holiday': False, 'frost': False,
    'function': 'Boost', 'boost': 'On', 'airflow': 'Heat recovery',
    'alarms': 0, 'alarm': 'None', 'fire_reply': 0,
    'rh_set': 65, 'boost_delay': 60, 'overrun_min': 20,
    'unit_fw': '1.23',
    'out_offset': 0.0,
    'ntp': {'on': False, 'tz': 0.0, 'rule': 4, 'text': 'Network time is off.'},
    'fans': {'in': [30.0, 50.0, 80.0], 'out': [28.5, 48.0, 78.0], 'stall': 20.0, 'holiday': 50.0},
    'night': {'on': True, 'start': 1320, 'end': 360},
    'filter': 1, 'hours': 4321, 'outages': 2,
    'device': {'name': 'Vectaire MVHR', 'id': 'a1b2c3', 'fw': '0.0-preview', 'hw': 'esp32c5 (display)',
               'ssid': 'HomeWiFi', 'wifi_trial': False, 'wifi_change': '', 'ip': '192.168.1.50', 'rssi': -58,
               'ha': 'connected', 'heap_free': 180000, 'heap_min': 150000},
}
state = copy.deepcopy(DEFAULT)

# the time zone list is the one in the firmware: components/mvhr_model/timezones.c
RULES = {'UK': 0, 'CENTRAL': 1, 'EAST': 2, 'US': 3, 'NONE': 4}
ZONES = [[m[0], int(m[1]), RULES[m[2]]] for m in re.findall(
    r'\{"([^"]+)", (-?\d+), DST_RULE_(\w+)\}',
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'components', 'mvhr_model', 'timezones.c')).read())]

BYPASS_MODE = {'auto': None, 'heat': 0, 'bypass': 1, 'mode3': 2}


def snapshot():
    s = copy.deepcopy(state)
    nt = s['ntp']
    nt['zone'] = next((i for i, z in enumerate(ZONES) if z[1] == round(nt['tz'] * 10) and z[2] == nt['rule']), -1)
    s['t_out_raw'] = s['t_out']
    s['t_out'] = round(s['t_out'] + s['out_offset'], 1)      # the controller adds the offset before showing it
    now = time.localtime()
    s['clock'] = {'h': now.tm_hour, 'mi': now.tm_min, 'd': now.tm_mday, 'mo': now.tm_mon, 'y': now.tm_year,
                  'f24': 1, 'datefmt': 1, 'region': 160, 'offset': 0x80}   # offset 0x80: top bit set, low bits 0
    s['device']['uptime_s'] = int(time.time() - START)
    if not s['online']:                      # like the controller: only device info when the unit is silent
        s = {'online': False, 'simulated': s['simulated'], 'device': s['device']}
    return s


def apply_set(body):
    """Mirror of /api/set: one or more keys, returns False if nothing recognised."""
    n = 0
    for k in ('fan', 'rh_set', 'boost_delay', 'overrun_min', 'holiday', 'bypass'):
        if k in body:
            state[k] = body[k]
            n += 1
    if 'bypass' in body:
        mode = BYPASS_MODE.get(body['bypass'])
        if mode is not None:
            state['airflow'] = ['Heat recovery', 'Bypass', 'Intake motor off'][mode]
    if isinstance(body.get('night'), dict):
        state['night'].update(body['night'])
        n += 1
    if isinstance(body.get('out_offset'), (int, float)) and -10 <= body['out_offset'] <= 10:
        state['out_offset'] = round(float(body['out_offset']), 1)
        n += 1
    if isinstance(body.get('ntp'), dict):
        nt = state['ntp']
        if isinstance(body['ntp'].get('on'), bool):
            nt['on'] = body['ntp']['on']
        if isinstance(body['ntp'].get('tz'), (int, float)) and -12 <= body['ntp']['tz'] <= 14:
            nt['tz'] = float(body['ntp']['tz'])
        zi = body['ntp'].get('zone')
        if isinstance(zi, int) and 0 <= zi < len(ZONES):
            nt['tz'] = ZONES[zi][1] / 10
            nt['rule'] = ZONES[zi][2]
        if body['ntp'].get('rule') in (0, 1, 2, 3, 4):
            nt['rule'] = body['ntp']['rule']
        nt['text'] = 'On. The first check runs shortly after start-up.' if nt['on'] else 'Network time is off.'
        n += 1
    if body.get('ntp_check') is True:
        if state['ntp']['on']:
            state['ntp']['text'] = 'Last check 0 s ago: the clock was right (+0 min).'
        n += 1
    if isinstance(body.get('fans'), dict):
        for side in ('in', 'out'):
            vals = body['fans'].get(side) or []
            for i, v in enumerate(vals[:3]):
                if isinstance(v, (int, float)) and 10 <= v <= 100:
                    state['fans'][side][i] = round(float(v), 1)
        n += 1
    if body.get('reset_filter'):
        state['filter'] = 0
        n += 1
    if 'clock' in body:
        n += 1
    return n > 0


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write('%s %s\n' % (self.command, self.path))

    def send(self, code, body, ctype='application/json'):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def page(self, name):
        try:
            with open(os.path.join(WWW, name), 'rb') as f:
                html = f.read()
            if name == 'home.html':                  # add the preview controls; the file on disk is untouched
                with open(PANEL, 'rb') as f:
                    html = html.replace(b'</body>', f.read() + b'</body>', 1)
            self.send(200, html, 'text/html; charset=utf-8')
        except OSError:
            self.send(404, 'not found', 'text/plain')

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ('/', '/index.html'):
            return self.page('home.html')
        if u.path == '/diag':
            return self.page('diag.html')
        if u.path == '/api/state':
            snap = snapshot()
            if 'zones' in u.query:
                snap['zones'] = ZONES
            return self.send(200, json.dumps(snap))
        if u.path == '/api/bus':                  # just enough for the Diagnostics page
            return self.send(200, json.dumps({'simulated': True, 'online': True, 'requests': 1200, 'replies': 1200,
                'timeouts': 0, 'failures': 0, 'rx_bytes': 40000, 'echo_frames': 1200, 'crc_errors': 0,
                'framing_errors': 0, 'uptime_s': 3600, 'idle_bytes': 0, 'last_reply_ms': 18, 'rx_high_pct': -1,
                'attempts': ''}))
        if u.path == '/api/crash':
            return self.send(200, json.dumps({'reset_reason': 'power-on', 'uptime_s': 3600, 'heap_free': 180000,
                'heap_min': 150000, 'heap_largest': 90000, 'psram_free': 0, 'ha': 'connected'}))
        if u.path == '/api/auth':
            return self.send(200, '{"ok":true}')
        if u.path == '/api/scan':
            return self.send(200, json.dumps([
                {'ssid': 'HomeWiFi', 'rssi': -55, 'secure': True, 'band5g': False},
                {'ssid': 'HomeWiFi-5G', 'rssi': -62, 'secure': True, 'band5g': True},
                {'ssid': 'Neighbour', 'rssi': -81, 'secure': True, 'band5g': False}]))
        if u.path == '/mock':
            q = parse_qs(u.query)
            if 'reset' in q:
                state.clear()
                state.update(copy.deepcopy(DEFAULT))
            for k, v in q.items():
                if k == 'reset':
                    continue
                try:
                    val = json.loads(v[0])
                except ValueError:
                    val = v[0]
                target, parts = state, k.split('.')        # "device.ha" sets state["device"]["ha"]
                for part in parts[:-1]:
                    target = target.setdefault(part, {})
                target[parts[-1]] = val
            return self.send(200, json.dumps(snapshot(), indent=1))
        self.send(404, 'not found', 'text/plain')

    def do_POST(self):
        length = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(length) if length else b''
        if self.path == '/api/set':
            try:
                ok = apply_set(json.loads(raw or b'{}'))
            except ValueError:
                return self.send(400, 'json', 'text/plain')
            return self.send(200, '{"ok":true}') if ok else self.send(400, 'nothing to change', 'text/plain')
        if self.path in ('/api/wifi', '/api/bus/txtest'):
            return self.send(200, '{"ok":true}')
        self.send(404, 'not found', 'text/plain')


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f'Preview at http://localhost:{port}/   (Ctrl+C to stop; edit components/web/www/home.html and refresh)')
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
