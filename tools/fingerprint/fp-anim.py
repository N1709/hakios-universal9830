#!/usr/bin/env python3
# fp-anim.py: animated live view of the fingerprint enroll loop (kern.redwood.tee "fp run 3 ...") from the phone's
# kernel message buffer over SSH. Green reports reading/candidate, not verified iOS authentication.
import argparse, re, struct, subprocess, sys, tempfile, time

SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3']
ART = r'''
           .-~~~~~~~~~-.
        .-'  .-~~~~~-.  '-.
      .'   .'  .---.  '.   '.
     /   .'  .'     '.  '.   \
    /   /   /  .---.  \   \   \
   |   |   |  /     \  |   |   |
   |   |   |  | (*) |  |   |   |
   |   |   |  \     /  |   |   |
   |   |    \  '---'  /    |   |
    \   \    '.     .'    /   /
     \   '.    '---'    .'   /
      '.   '-.       .-'   .'
        '-.   '~---~'   .-'
           '-.._____..-'
'''.strip('\n').split('\n')
COL = {'white': '\033[1;97m', 'green': '\033[1;92m', 'red': '\033[1;91m', 'grey': '\033[90m'}
TXT = {'match': 'SENSOR MATCH -- token unverified', 'nomatch': 'NO MATCH -- lift and retry',
       'white': 'PRESS the sensor', 'green': 'HOLD...  reading your finger', 'red': 'NOT GOOD -- lift your finger',
       'grey': 'waiting for the phone...', 'done': 'ENROLLED  -- all done'}
RST = '\033[0m'
LINE = re.compile(r'redwood-tee: (fp run .*)$')

state, progress, good, bad, red_until, last_t = 'grey', 0, 0, 0, 0.0, -1.0
msg = ''
mode = 0


def draw():
    c = {'done': 'green', 'match': 'green', 'nomatch': 'red'}.get(state, state)
    title = {3: 'REGISTERING YOUR THUMB', 6: 'MATCHING SAVED THUMB'}.get(mode, 'WAITING FOR TEST')
    out = ['\033[H\033[2J', '\n      ' + title + '\n\n']
    for l in ART:
        out.append('      ' + COL[c] + l + RST + '\n')
    out.append('\n      ' + COL[c] + TXT[state].center(34) + RST + '\n\n')
    if mode == 3:
        bar = max(0, min(30, int(progress * 30 / 100)))
        out.append('      [' + COL['green'] + '#' * bar + RST + '.' * (30 - bar) + '] %3d%%\n' % progress)
    elif mode == 6:
        out.append('      Sensor test; iOS unlock is not enabled\n')
    out.append('      good %d   retries %d\n' % (good, bad))
    if msg:
        out.append('\n      ' + COL['grey'] + msg + RST + '\n')
    sys.stdout.write(''.join(out))
    sys.stdout.flush()


def event(text):
    global state, progress, good, bad, red_until, msg, mode
    now = time.time()
    start = re.search(r'^fp run ([36])(?: step (\d+):|:)', text)
    if start:
        incoming = int(start.group(1))
        if incoming != mode or start.group(2) == '1':
            if incoming != mode or incoming == 6:
                progress, good, bad = 0, 0, 0
            mode, state, msg = incoming, 'white', 'hold until the result, then lift'
    if 'fp run 6: SENSOR MATCH candidate' in text:
        good += 1
        state, red_until, msg = 'match', now + 2.0, 'lift; press again to test'
        return
    if 'fp run 6: NO MATCH' in text or ('fp run 6: bad image (0x20)' in text):
        bad += 1
        state, red_until, msg = 'nomatch', now + 2.0, 'lift; press again'
        return
    if 'invalid response' in text or 're-arm failed' in text:
        state, msg = 'grey', 'protocol error: test stopped'
        return
    if 'SAMPLE' in text and 'ACCEPTED' in text:
        good += 1
        m = re.search(r'progress (\d+)', text)
        progress = int(m.group(1)) if m else progress
        state = 'done' if progress >= 100 else 'green'
        msg = 'good one -- now LIFT your finger'
    elif 'error 0x27' in text or 'capture rejected' in text or 'bad image' in text:
        if state != 'red':
            bad += 1
        state, red_until = 'red', now + 1.2
        msg = 'lift, move a little, press again (flat and steady)'
        m = re.search(r'quality ([0-9a-f]{8})', text)
        if m:
            q = int(m.group(1), 16)
            # libbauthserver finger_image_quality_map
            for bit, why in ((0x08000000, 'only part of your finger: cover the whole spot'),
                             (0x04000000, 'too light: press a little harder'),
                             (0x01000000, 'finger too wet: dry it'),
                             (0x2000, 'screen dirty: wipe the spot'),
                             (0x8, 'same as last time: lift and move your finger a little'),
                             (0x2, 'too fast: hold until it turns green'),
                             (0x100, 'too slow')):
                if q & bit:
                    msg = why
                    break
    elif 'ON THE SENSOR' in text or 'finger held' in text:
        if now >= red_until:
            state, msg = 'green', ''
    elif 'touch at' in text:
        if now >= red_until:
            state, msg = 'white', 'off the spot: centred, about 3 cm up from the bottom edge'
    elif 'finger lifted' in text or ('-> result 0' in text and re.search(r' op 4 error 0 ', text)):
        if state != 'done':
            state, msg = ('white', '') if now >= red_until else (state, msg)
    elif 'steps, last op' in text:
        if state != 'done':
            state, msg = 'grey', 'run ended'


class LogCursor:
    """Consume new bytes only; do not replay old matches when opening the view."""
    def __init__(self):
        self.total = None
        self.partial = b''
        self.rebooted = False

    def feed(self, snapshot):
        if len(snapshot) < 8:
            raise ValueError('Incomplete message-buffer snapshot')
        total = struct.unpack_from('<Q', snapshot)[0]
        data = snapshot[8:]
        self.rebooted = self.total is not None and total < self.total
        if self.total is None or self.rebooted:
            self.total, self.partial = total, b''
            return []
        delta, self.total = total - self.total, total
        if not delta:
            return []
        if delta > len(data):
            self.partial = b''
            payload = data.split(b'\n', 1)[-1]  # skip the overwritten partial line
        else:
            payload = data[-delta:]
        lines = (self.partial + payload).split(b'\n')
        self.partial = lines.pop()
        return [line.decode('latin1', 'replace') for line in lines]


def run_view(connection, duration=0):
    global state, msg, red_until, progress, good, bad
    cursor = LogCursor()
    deadline = time.monotonic() + duration if duration else float('inf')
    draw()
    while time.monotonic() < deadline:
        try:
            result = subprocess.run(connection, capture_output=True, timeout=6)
            if result.returncode or len(result.stdout) < 8:
                raise OSError('phone message buffer unavailable')
            lines = cursor.feed(result.stdout)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            state, msg = 'grey', 'connection interrupted; reconnecting'
            draw()
            time.sleep(1)
            continue
        if cursor.rebooted:
            state, progress, good, bad = 'grey', 0, 0, 0
            msg = 'phone restarted; waiting for a new test'
        elif state == 'grey' and msg in ('', 'connection interrupted; reconnecting'):
            state, msg = 'white', 'connected; waiting for test'
        for line in lines:
            match = LINE.search(line)
            if match:
                event(match.group(1))
        if state in ('red', 'match', 'nomatch') and time.time() >= red_until:
            state, msg = 'white', 'lift, then press when the next test touch is ready'
        draw()
        time.sleep(0.4)
    sys.stdout.write(RST + '\nView finished.\n')


def main(duration=0):
    with tempfile.TemporaryDirectory(prefix='fp-view-', dir='/tmp') as directory:
        control = directory + '/ssh'
        connection = SSH + ['-o', 'ControlMaster=auto', '-o', 'ControlPersist=10',
                            '-o', 'ControlPath=' + control, 's20',
                            '/var/jb/usr/sbin/sysctl -b kern.redwood_dmesg']
        try:
            run_view(connection, duration)
        finally:
            # Close only this view's private SSH master, never another agent's.
            try:
                subprocess.run(SSH + ['-S', control, '-O', 'exit', 's20'],
                               capture_output=True, timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                pass


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Live fingerprint research results over SSH; no log-daemon restart')
    parser.add_argument('--duration', type=float, default=0, help='stop the view after this many seconds')
    args = parser.parse_args()
    try:
        main(args.duration)
    except KeyboardInterrupt:
        sys.stdout.write(RST + '\n')
