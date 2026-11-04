"""Two-copy test driver for the online duel (developer tool).

Copy A is started through the normal launcher, copy B with run2.py, a local helper that isn't in the repository
(the game allows one copy at a time).
Every step checks that it actually happened within a few seconds and stops with a clear message if not.
Clicks are injected through the mod's test pipe (mods\\debug.txt must exist), so they work whether or not the
windows have focus and wherever the real mouse is.

  python duo.py start            close running copies, start A and B, get both to the main menu
  python duo.py connect          A hosts, B joins 127.0.0.1, A picks the first level
  python duo.py duel             A picks the crocodile, B picks Jimmy Lightning, A presses Play Now
  python duo.py autoplay N       whoever is in control fires at random spots, N shots or until the results
  python duo.py synced NAME      pause both at the same tick, screenshot both; fails if any pixel differs
  python duo.py chat             A and B chat during the duel; clicking away from the phrase list fires no shot
  python duo.py localfirst       A plays a short local duel and returns to the main menu
  python duo.py scenario         chat -> results (chat) -> Replay Level -> results -> Play New Level -> new level
                                 -> B leaves
  python duo.py full             start, localfirst, connect, duel, scenario
  python duo.py pad              the same with a (pretend) controller: Options > CONTROLLER and remapping, Duel >
                                 Online, the address on the on-screen keyboard, masters, aiming and shooting with the
                                 sticks, chat, the Menu, the results, leaving
  python duo.py padfull          start, pad
  python duo.py local            copy A: a saved replay from Replays, then a local duel with two (pretend)
                                 controllers: each player's own highlight on the selection, each turn played only by
                                 its player's controller (aim, fast-forward, shoot, vibration), Instant Replay's
                                 panel, swapping the controllers in CONTROLLER, one controller playing both, leaving
  python duo.py replays          copy A: just the saved replay
  python duo.py display          copy A: the window scaled to the monitor's text size and drawn by the mod, the mouse
                                 kept in over the whole picture (a pretend position), Custom Cursors drawn in place,
                                 then Options > BORDERLESS on the smallest monitor (covers it for a minute): the
                                 picture in shape with black bars, the mouse mapped and kept in, Fullscreen unticking
                                 it, Done keeping it
  python duo.py displayfull      start, display
  python duo.py localfull        start, local
  python duo.py chaos            copy A: Chaos Mode in a local duel: the CHAOS MODE box on the selection (ticks,
                                 remembered), the rounds (two shots without an effect, then a new announced effect
                                 every round), a Zen Ball (Master Hu's power) being a shot of its round, then each of
                                 the 16 effects checked by what it does, and the Spooky Ball coming back (at the
                                 bottom when upside down); masters' powers at the bucket (chaospowers); then a shot
                                 under each effect and its Instant Replay (the effect in place, the balls just where
                                 they went), the replay panel's Save keeping the effects in the file
  python duo.py chaosreplay [N...]  just the Instant Replays (of effects N...)
  python duo.py chaoszen         just the rounds and the Zen Ball
  python duo.py chaospowers      just the masters' powers at the bucket with its effects: Claude's flippers upside
                                 down (at the top, swinging down, batting the ball down), Double Bucket keeping half
                                 the track over the turns, the second bucket's rim, catch and lid (a fireball knocks
                                 it off), Chain Lightning's arc to the nearer bucket and upside down to the rod under
                                 the bucket, the pyramid's sides and a catch upside down (chaospowersfull: start
                                 first)
  python duo.py chaosonline      A hosts, B joins: the joiner's box shows the host's; online Chaos Mode duels (Replay
                                 Level) through all 16 effects, the same on both, pictures identical, the bomb set off
  python duo.py chaosfull | chaosonlinefull   start first
  python duo.py redpeg           copy A: Red Peg Mode in a local duel: the RED PEG MODE box above CHAOS MODE (ticks,
                                 remembered), one red peg (never made purple), drawn red as a brick, a moving peg and
                                 a round one; hitting it shows "Sabotage!" and gives the other player each of its
                                 effects for their next turn only (Mirage: the board black as the turn passes, fakes in
                                 gone pegs' places, the guide stopping at a fake, fakes glitching when the ball goes
                                 through, again in its shot's Instant Replay; Silhouette's gray pegs, Blind Shot,
                                 Lights Out), no red peg after it's hit, the replay of the hit not giving it again;
                                 then with Chaos Mode too: both effects at once, announced together, never the same
                                 one; Mirage's fakes on Eiffel Power; a red brick turning round a flower (The
                                 Guardians) drawn red in every frame
  python duo.py redturning       just the turning red brick
  python duo.py redpegonline     A hosts, B joins: the joiner's RED PEG MODE box shows the host's; an online duel with
                                 the red peg hit and Mirage then Silhouette, the same on both, pictures identical;
                                 SAVE in Instant Replay's place (B's shot saved, noted in B's chat only, one saved
                                 in flight after the shot), still in sync; the saved shot played from Replays
  python duo.py redpegfull | redpegonlinefull   start first
  python duo.py finish           close both copies and turn debug mode off (start turns it on)
  python duo.py shot NAME | click A|B X Y | settext A|B ID TEXT | state | log A|B [N]
"""
import json
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
GAME = os.path.dirname(MOD)
STATE = os.path.join(HERE, 'duo.json')
SHOTS = os.path.join(MOD, 'shots')
sys.path.insert(0, HERE)
import dbg  # noqa: E402
import gamectl  # noqa: E402

# Our dialogs (online.cpp)
DLG_MODE, DLG_ROLE, DLG_ADDRESS, DLG_HOSTING, DLG_JOINING, DLG_WAIT, DLG_NOTICE = 9001, 9002, 9003, 9004, 9005, 9006, 9007
# The game's own dialogs
DLG_GAME_MENU, DLG_LEAVE_GAME = 19, 4  # in-game Menu, and its "Leave Game?" confirmation
DLG_SELECTION = 1                      # master selection
DLG_CONTINUE_GAME = 25                 # "Continue Game?" when a left local duel was saved
LEAVE_ONLINE_TEXT = 'Leaving ends the online duel for both players.  Do you want to continue?'
RESULTS_SCORES = (180, 200, 630, 280)  # the results dialog counts the scores up on each computer's own clock


class StepFailed(Exception):
    pass


# ---- plumbing ----

def load():
    with open(STATE) as f:
        return json.load(f)


def pid(k):
    return load()[k]['pid']


def send(k, cmd, timeout=15):
    return dbg.send(pid(k), cmd, timeout=timeout)


def click(k, x, y):
    send(k, f'click {x} {y}')


def frame(k, name='check'):
    """The game's current frame (from its draw surface, so covering windows don't matter)."""
    from PIL import Image
    bmp = os.path.join(SHOTS, f'_{k}.bmp')
    if send(k, f'shot 0 {bmp}') != 'ok':
        raise StepFailed(f'copy {k} could not capture its frame')
    im = Image.open(bmp).convert('RGB')
    im.save(os.path.join(SHOTS, f'{name}_{k}.png'))
    return im


def state(k):
    return dict(kv.split('=') for kv in send(k, 'state').split() if '=' in kv)


def log_path(k):
    return os.path.join(GAME, 'mods', 'logs', f'netplay-{pid(k)}.log')


def log_lines(k):
    with open(log_path(k)) as f:
        return f.read().splitlines()


def mark():
    """Current log length of both copies; wait_log() only looks at lines written after it."""
    return {k: len(log_lines(k)) for k in 'AB'}


def wait(check, timeout, what):
    end = time.time() + timeout
    while True:
        try:
            result = check()
        except OSError:  # test pipe not up yet
            result = None
        if result:
            return result
        if time.time() > end:
            raise StepFailed(what)
        time.sleep(0.4)


def wait_log(k, text, since, timeout, what=None):
    def found():
        return any(text in line for line in log_lines(k)[since[k]:])
    wait(found, timeout, what or f'copy {k} never logged "{text}"')


def dialog_open(k, dlg):
    return send(k, f'dialog {dlg}') == 'open'


def settled(k, box, timeout, what):
    """Wait until part of copy k's picture stops changing (dialog animations are not part of the shared game)."""
    from PIL import ImageChops
    last = [None]

    def unchanged():
        cur = frame(k, 'settle').crop(box)
        same = last[0] is not None and ImageChops.difference(last[0], cur).getbbox() is None
        last[0] = cur
        return same
    wait(unchanged, timeout, what)


def leave_board(k):
    """In-game Menu > Main Menu > Leave."""
    click(k, 38, 560)                                        # Menu
    wait(lambda: dialog_open(k, DLG_GAME_MENU), 5, f'copy {k}: Menu did not open')
    time.sleep(1.2)                                          # slides in
    online = send(k, 'state') != 'inactive'
    click(k, *(button_center(k, 'Main Menu') or (306, 449)))  # Main Menu (it shares its row with CONTROLLER)
    wait(lambda: dialog_open(k, DLG_LEAVE_GAME) or not dialog_open(k, DLG_GAME_MENU), 5,
         f'copy {k}: Main Menu did nothing')
    if dialog_open(k, DLG_LEAVE_GAME):                       # the game doesn't always ask
        time.sleep(1.2)
        if online and send(k, f'findstr {DLG_LEAVE_GAME} {LEAVE_ONLINE_TEXT}') == 'none':
            raise StepFailed(f'copy {k}: "Leave Game?" still says the game will be saved')
        click(k, *(button_center(k, 'Leave') or (327, 424)))  # Leave (placed by the Menu's size)
    wait(lambda: not dialog_open(k, DLG_GAME_MENU) and not dialog_open(k, DLG_LEAVE_GAME), 5,
         f'copy {k}: Leave did not close the menu')


def can_shoot():
    """Someone is in control and aiming."""
    return any(s.get('control') == '1' and s.get('state') == '1' for s in (state(k) for k in 'AB'))


def chat_state(k):
    head, _, last = send(k, 'chat').partition(' last=')
    s = dict(kv.split('=') for kv in head.split() if '=' in kv)
    s['last'] = last
    return s


def say(k, phrase):
    """Copy k says a set phrase (its index in the list) through the CHAT button."""
    bx, by = map(int, chat_state(k)['button'].split(','))
    click(k, bx, by)
    wait(lambda: chat_state(k)['open'] == '1', 3, f'copy {k}: CHAT did not open the phrase list')
    x, y = map(int, send(k, f'chatrow {phrase}').split())
    click(k, x, y)
    wait(lambda: chat_state(k)['open'] == '0', 3, f'copy {k}: clicking a phrase did not close the list')


def chat(name, a_says, b_says):
    """A and B each say something: both must see both lines, and the box must then fade by itself."""
    for k, other, phrase in (('A', 'B', a_says), ('B', 'A', b_says)):
        say(k, phrase)
        text = send(k, 'chat').partition(' last=')[2]
        wait(lambda: chat_state(other)['last'] == text, 5, f'copy {other} never showed "{text}" from {k}')
    if any(chat_state(k)['alpha'] != '255' for k in 'AB'):
        raise StepFailed('the chat box is not showing after the messages')
    for k in 'AB':
        frame(k, name)
    wait(lambda: all(chat_state(k)['alpha'] == '0' for k in 'AB'), 8, 'the chat box did not fade out by itself')
    ok(f'{name}: both players chatted, the box faded')


def chat_click_away():
    """The player who is aiming opens the phrase list and clicks the board next to it: the list closes, no shot."""
    k = next((k for k in 'AB' if state(k).get('control') == '1' and state(k).get('state') == '1'), None)
    if not k:
        raise StepFailed('no one is aiming')
    before = state(k)
    bx, by = map(int, chat_state(k)['button'].split(','))
    click(k, bx, by)
    wait(lambda: chat_state(k)['open'] == '1', 3, f'copy {k}: CHAT did not open the phrase list')
    click(k, 500, 400)
    wait(lambda: chat_state(k)['open'] == '0', 3, f'copy {k}: clicking away did not close the phrase list')
    time.sleep(1.5)
    after = state(k)
    if (after['state'], after['balls']) != (before['state'], before['balls']):
        raise StepFailed(f'copy {k}: closing the phrase list fired a shot')
    wait(lambda: chat_state(k)['alpha'] == '0', 8, 'the chat box did not fade out by itself')
    ok(f'copy {k}: clicking away from the phrase list only closed it')


def ok(msg):
    print('OK:', msg, flush=True)


# ---- the controller (a pretend one, through the test pipe) ----

def pad_state(k):
    """The controller's view: context (menu, board, chat, keyboard, ...), focus=class,x,y,row,label, and more."""
    s = {}
    for kv in send(k, 'padstate').split():
        if '=' in kv:
            key, _, value = kv.partition('=')
            s.setdefault(key, value)
    return s


def pad(k, buttons='-', lx=0, ly=0, rx=0, ry=0, n=1):
    """Pretend controller n (1 or 2) of copy k in this state."""
    send(k, f'pad {n} {buttons} {lx} {ly} {rx} {ry}')


def tap(k, button, hold=0.12, n=1):
    """Press and let go, as a player would."""
    pad(k, button, n=n)
    time.sleep(hold)
    pad(k, n=n)
    time.sleep(0.2)


def focus_center(k):
    f = pad_state(k)['focus'].split(',')
    return (int(f[1]), int(f[2])) if len(f) > 2 else None


def pad_goto(k, x, y, what, steps=16):
    """Move the highlight with the D-pad until it is on the target at (x, y) (within a few pixels): along the
    longer way first, the other way from anywhere it has already been (rows of different lengths)."""
    pad(k)
    been = set()
    for _ in range(steps):
        at = focus_center(k)
        if at and abs(at[0] - x) <= 6 and abs(at[1] - y) <= 6:
            return
        if at is None:
            tap(k, 'DOWN')  # brings the highlight up
            continue
        dx, dy = x - at[0], y - at[1]
        on_slider = pad_state(k)['focus'].startswith('ThunderSlider')  # left/right would move the slider
        sideways = abs(dx) > abs(dy) and not (on_slider and abs(dy) > 6)
        if at in been and abs(dx) > 6 and abs(dy) > 6 and not on_slider:
            sideways = not sideways
        been.add(at)
        tap(k, ('RIGHT' if dx > 0 else 'LEFT') if sideways else ('DOWN' if dy > 0 else 'UP'))
    raise StepFailed(f'copy {k}: the controller could not reach {what} (highlight at {focus_center(k)})')


def pad_press(k, x, y, what):
    pad_goto(k, x, y, what)
    tap(k, 'A')


def button_centers(k, label):
    """Where the buttons with that lettering are (from the widget dump)."""
    out = []
    for line in send(k, 'widgets').splitlines():
        s = line.strip()
        if s.endswith(f'"{label}"') and ' vm' in s:
            parts = s.split()
            x, y = map(int, parts[1].split(','))
            w, h = map(int, parts[2].split('x'))
            out.append((x + w // 2, y + h // 2))
    return out


def button_center(k, label):
    """Where the (first) button with that lettering is, or None."""
    found = button_centers(k, label)
    return found[0] if found else None


def pad_press_label(k, label):
    at = button_center(k, label)
    if not at:
        raise StepFailed(f'copy {k}: no "{label}" button on screen')
    pad_press(k, *at, f'"{label}"')


def focus_label(k):
    f = pad_state(k)['focus'].split(',', 4)
    return f[4].replace('_', ' ') if len(f) > 4 else ''


OSK_ROWS = ['1234567890', 'QWERTYUIOP', 'ASDFGHJKL.', 'ZXCVBNM-:_']


def osk_type(k, text):
    """Type on the on-screen keyboard with the D-pad and A (keyboard.cpp's layout: 4 rows of 10, then specials)."""
    for ch in text.upper():
        row = next(r for r in range(4) if ch in OSK_ROWS[r])
        col = OSK_ROWS[row].index(ch)
        for _ in range(30):
            key = int(pad_state(k)['key'])
            r, c = divmod(key, 10) if key < 40 else (4, None)
            if r == row and c == col:
                break
            if r != row:
                tap(k, 'DOWN' if row > r else 'UP')
            else:
                tap(k, 'RIGHT' if col > c else 'LEFT')
        else:
            raise StepFailed(f'copy {k}: could not reach key {ch} on the on-screen keyboard')
        tap(k, 'A')


# ---- steps ----

def title_button_showing(k):
    r, g, b = frame(k, 'title').getpixel((400, 520))
    return g > 150 and b < 120  # the "Click to Play" button, or the loading bar under it


DEBUG_FLAG = os.path.join(GAME, 'mods', 'debug.txt')


def close_all():
    gamectl.main(['closeall'])
    end = time.time() + 15
    while time.time() < end and any(w[3] == 'MainWindow' for w in gamectl.game_windows()):
        time.sleep(0.3)


def finish():
    """Close both copies and turn the mod's debug mode off again (normal play doesn't need the test pipe)."""
    close_all()
    if os.path.exists(DEBUG_FLAG):
        os.remove(DEBUG_FLAG)
    ok('copies closed, debug mode off')


def start():
    if not os.path.exists(DEBUG_FLAG):  # the test pipe and diagnostics
        with open(DEBUG_FLAG, 'w') as f:
            f.write('Enables the online mod\'s test pipe and diagnostics. Delete for normal play.\n')
    gamectl.main(['closeall'])
    end = time.time() + 15
    while time.time() < end and any(w[3] == 'MainWindow' for w in gamectl.game_windows()):
        time.sleep(0.3)
    for w in gamectl.game_windows():  # test copies stuck behind a dialog
        subprocess.run(['taskkill', '/F', '/PID', str(w[1])], capture_output=True)
    # The previous copy A's launcher holds a single-instance mutex until it exits.
    for _ in range(20):
        out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq PeggleNights.exe', '/NH'], capture_output=True,
                             text=True).stdout
        if 'PeggleNights.exe' not in out:
            break
        time.sleep(0.5)
    else:
        subprocess.run(['taskkill', '/F', '/IM', 'PeggleNights.exe'], capture_output=True)
    # A crashed or killed copy leaves the game's "session in progress" flag set, which makes the next start
    # stop at a hardware-acceleration question. Clear it, as a clean exit would have.
    subprocess.run(['reg', 'add', r'HKCU\Software\Steam\PeggleNights', '/v', 'InProgress', '/t', 'REG_DWORD',
                    '/d', '0', '/f'], capture_output=True)
    # Every test starts without Chaos Mode or Red Peg Mode ticked (their tests tick them).
    import ctypes
    for key in ('chaos', 'red'):
        ctypes.windll.kernel32.WritePrivateProfileStringW('duel', key, '0', os.path.join(GAME, 'mods', 'netplay.ini'))

    out = subprocess.run(['powershell', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(HERE, 'launch.ps1')],
                         capture_output=True, text=True).stdout
    a = dict(kv.split('=') for kv in out.split() if '=' in kv)
    if 'pid' not in a:
        raise StepFailed(f'copy A did not start: {out.strip()}')
    if not os.path.exists(os.path.join(HERE, 'run2.py')):
        raise StepFailed('copy B needs mod/tools/run2.py, a local helper that is not in the repository')
    out = subprocess.run([sys.executable, os.path.join(HERE, 'run2.py')], capture_output=True, text=True).stdout
    b = dict(kv.split('=') for kv in out.split() if '=' in kv)
    if 'pid' not in b or b.get('hwnd') == '?':
        raise StepFailed(f'copy B did not start: {out.strip()}')
    st = {'A': {'pid': int(a['pid']), 'hwnd': int(a['hwnd'])}, 'B': {'pid': int(b['pid']), 'hwnd': int(b['hwnd'])}}
    with open(STATE, 'w') as f:
        json.dump(st, f)
    gamectl.main(['place', str(st['A']['hwnd']), '850', '300'])
    gamectl.main(['place', str(st['B']['hwnd']), '1670', '300'])
    ok(f"started A (pid {st['A']['pid']}) and B (pid {st['B']['pid']})")

    for k in 'AB':  # the test pipe comes up with the first drawn frame
        wait(lambda: send(k, 'ping') == 'pong', 30, f'copy {k} test pipe never answered')
    for k in 'AB':
        wait(lambda: title_button_showing(k), 40, f'copy {k} never showed its title screen')

    def past_title(k):
        if not title_button_showing(k):
            return True
        click(k, 400, 520)  # does nothing while still loading
        return False
    for k in 'AB':
        wait(lambda: past_title(k), 40, f'copy {k} stayed on its title screen')
    time.sleep(1.5)  # main menu fades in
    ok('both copies at the main menu')


def connect(level=(140, 170)):
    send('B', 'setname Dad')  # both copies share one profile; tell them apart
    since = mark()
    for k in 'AB':
        click(k, 680, 372)                                   # Duel
    for k in 'AB':
        wait(lambda: dialog_open(k, DLG_MODE), 5, f'copy {k}: Duel did not open the Local/Online dialog')
    time.sleep(0.8)
    for k in 'AB':
        click(k, 360, 286)                                   # Online Play
    for k in 'AB':
        wait(lambda: dialog_open(k, DLG_ROLE), 5, f'copy {k}: Online Play did not open Host/Join')
    time.sleep(0.8)
    click('A', 220, 304)                                     # Host Game
    wait_log('A', 'net: listening', since, 5)
    click('B', 363, 304)                                     # Join Game
    wait(lambda: dialog_open('B', DLG_ADDRESS), 5, 'copy B: Join Game did not open the address box')
    time.sleep(0.8)
    send('B', f'settext {DLG_ADDRESS} 127.0.0.1')
    click('B', 327, 348)                                     # OK
    for k in 'AB':
        wait_log(k, 'online: opponent is', since, 10)
    ok('connected (A hosts, B joined 127.0.0.1)')
    time.sleep(1.5)  # level screen slides in on A
    level_page('A', 1)
    since = mark()
    click('A', *level)
    for k in 'AB':
        wait_log(k, 'lockstep: begin', since, 10)
    ok('both games built the same level')
    wait(lambda: all(state(k).get('seldlg') == '1' for k in 'AB'), 15, 'the master selection never appeared on both')
    ok('master selection showing on both')


def duel():
    since = mark()
    click('A', 271, 391)   # A: the crocodile (left column)
    click('B', 600, 173)   # B: Jimmy Lightning (right column)
    time.sleep(1)
    click('A', 400, 547)   # A: Play Now
    for k in 'AB':
        wait_log(k, 'duel started', since, 10)
    ok('duel started on both')


def autoplay(shots, seed=1):
    """Whoever is in control fires at a random spot in their aiming phase; stops at the results screen."""
    rnd = random.Random(seed)
    fired = 0
    last_tick = None
    last_progress = time.time()
    while fired < shots:
        s = {k: state(k) for k in 'AB'}
        if any(v.get('enddlg') == '1' for v in s.values()):
            ok(f'results screen up after {fired} shots')
            return 'results'
        if any(v.get('desync') == '1' for v in s.values()):
            raise StepFailed(f'the games went out of sync: {s}')
        shooter = next((k for k in 'AB' if s[k].get('control') == '1' and s[k].get('state') == '1'
                        and s[k].get('seldlg') == '0'), None)
        if shooter and s[shooter]['tick'] != last_tick:
            x, y = rnd.randint(120, 680), rnd.randint(200, 560)
            send(shooter, f'move {x} {y}')
            time.sleep(0.3)
            click(shooter, x, y)
            fired += 1
            last_tick = s[shooter]['tick']
            last_progress = time.time()
            print(f'  shot {fired}: {shooter} fires at ({x},{y})', flush=True)
            time.sleep(2)
        elif time.time() - last_progress > 60:
            raise StepFailed(f'no one could shoot for 60 s: {s}')
        time.sleep(0.5)
    ok(f'{fired} shots played')
    return 'shots'


def synced(name):
    """Freeze both games at the same tick, screenshot both and count the pixels that differ."""
    from PIL import ImageChops
    for _ in range(5):
        ticks = [int(send(k, 'tick')) for k in 'AB']
        target = max(ticks) + 60
        for k in 'AB':
            send(k, f'hold {target}')
        # A copy whose frames are slow can be past the target by the time its hold arrives: aim again.
        if all(int(send(k, 'tick')) <= target for k in 'AB'):
            break
        for k in 'AB':
            send(k, 'hold')
    try:
        wait(lambda: all(int(send(k, 'tick')) == target for k in 'AB'), 15, '')
    except StepFailed:
        raise StepFailed(f'the games did not reach tick {target}: ' + ' / '.join(send(k, 'state') for k in 'AB'))
    a, b = frame('A', name), frame('B', name)
    for k in 'AB':
        send(k, 'hold')
    d = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v > 40 else 0)
    diff = sum(1 for p in d.getdata() if p)
    if diff:
        raise StepFailed(f'{name}: at tick {target} the two pictures differ in {diff} pixels, within {d.getbbox()} '
                         f'(see {name}_A.png, {name}_B.png)')
    ok(f'{name}: identical pictures at tick {target}')


def localfirst():
    """Copy A plays a short local duel, then returns to the main menu (state carried into an online duel)."""
    click('A', 680, 372)                                     # Duel
    wait(lambda: dialog_open('A', DLG_MODE), 5, 'Duel did not open the Local/Online dialog')
    time.sleep(0.8)
    click('A', 215, 286)                                     # Local Play
    time.sleep(1.5)
    if dialog_open('A', DLG_CONTINUE_GAME):                  # an earlier local duel was left and saved
        time.sleep(0.8)
        click('A', 440, 297)                                 # New Game
        wait(lambda: not dialog_open('A', DLG_CONTINUE_GAME), 5, 'copy A: New Game did not close "Continue Game?"')
    time.sleep(2.5)
    level_page('A', 1)
    click('A', 140, 170)                                     # first level
    wait(lambda: dialog_open('A', DLG_SELECTION), 10, 'copy A: the local duel never reached the master selection')
    time.sleep(1.5)                                          # slides in
    click('A', 400, 547)                                     # Play Now
    wait(lambda: not dialog_open('A', DLG_SELECTION), 5, 'copy A: Play Now did not start the local duel')
    time.sleep(4)
    for i in range(3):                                       # A plays both sides
        x = 250 + 100 * i
        send('A', f'move {x} 400')
        time.sleep(0.3)
        click('A', x, 400)
        time.sleep(12)
    leave_board('A')
    time.sleep(3)
    frame('A', 'localfirst')
    ok('copy A played a local duel and returned to the main menu')


def results_settled():
    for k in 'AB':
        settled(k, RESULTS_SCORES, 30, f'copy {k}: the results never finished counting up')


def scenario():
    chat('chat_duel', 0, 3)                                  # "Nice shot!", "Lucky!"
    chat_click_away()
    if autoplay(40, seed=7) != 'results':
        raise StepFailed('the duel did not reach the results screen')
    results_settled()
    synced('results1')
    chat('chat_results', 9, 10)                              # "Good game!", "Rematch?" over the results dialog
    since = mark()
    click('A', 513, 413)                                     # host: Replay Level
    wait_log('A', 'results button 1001 pressed (host decides)', since, 5)
    wait(lambda: all(state(k).get('seldlg') == '1' for k in 'AB'), 20,
         'Replay Level did not bring back the master selection on both (stuck in the intro?)')
    ok('Replay Level restarted the level; master selection showing on both')
    time.sleep(2)  # selection slides in
    duel()
    wait(can_shoot, 15, 'after Play Now no one gets to shoot (control never handed over?)')
    autoplay(3, seed=8)
    time.sleep(6)
    synced('replay_play')
    if autoplay(40, seed=9) != 'results':
        raise StepFailed('the second duel did not reach the results screen')
    results_settled()
    since = mark()
    click('A', 327, 453)                                     # host: Play New Level
    wait_log('B', 'lockstep: end', since, 10)
    wait(lambda: dialog_open('B', DLG_WAIT), 10, 'B is not waiting for the host after Play New Level')
    ok('Play New Level: A is choosing, B is waiting')
    time.sleep(2)
    since = mark()
    click('A', 600, 170)                                     # host picks another level
    for k in 'AB':
        wait_log(k, 'lockstep: begin', since, 10)
    wait(lambda: all(state(k).get('seldlg') == '1' for k in 'AB'), 15, 'the selection did not appear for the new level')
    duel()
    autoplay(2, seed=10)
    time.sleep(6)
    synced('newlevel_play')
    since = mark()
    leave_board('B')                                         # the joiner leaves mid-duel
    wait_log('B', 'leaving session', since, 10)
    wait_log('A', 'left the game', since, 10, 'copy A was not told that B left')
    ok('B left; A was told')


# ---- the same, played with the controller ----

def pad_menus():
    """Copy A: Options > CONTROLLER: give SHOOT another button (the action that had it swaps), reset, back out."""
    k = 'A'
    pad(k)
    time.sleep(0.2)
    pad_press_label(k, 'Options')
    wait(lambda: button_center(k, 'Controller'), 5, 'the Options dialog has no CONTROLLER button')
    time.sleep(1.2)                                          # slides in
    if not pad_state(k)['focus'].startswith('ThunderSlider'):
        raise StepFailed(f'Options should start on its first slider, not {pad_state(k)["focus"]}')
    frame(k, 'pad_options')
    pad_press_label(k, 'Controller')
    wait(lambda: pad_state(k)['dialog'] == '1', 5, 'CONTROLLER did not open the controller settings')
    time.sleep(1.2)
    frame(k, 'pad_settings')
    if focus_label(k) != 'SHOOT / SELECT: A / CROSS':
        raise StepFailed(f'the settings should start on SHOOT, not "{focus_label(k)}"')
    tap(k, 'A')
    wait(lambda: pad_state(k)['capturing'] == '0', 3, 'choosing SHOOT did not wait for a button')
    tap(k, 'X')
    wait(lambda: pad_state(k)['bindings'].startswith('X,B,Y,A,'), 3,
         f'X did not become SHOOT with Instant Replay taking A: {pad_state(k)["bindings"]}')
    pad_goto(k, *button_center(k, 'RESET TO DEFAULTS'), 'RESET TO DEFAULTS')
    tap(k, 'X')                                              # X is SHOOT / SELECT now
    wait(lambda: pad_state(k)['bindings'] == 'A,B,Y,X,RT,START,LB+RB', 3, 'RESET TO DEFAULTS did not reset')
    tap(k, 'B')
    wait(lambda: pad_state(k)['dialog'] == '0', 3, 'B did not close the controller settings')
    time.sleep(1)
    if focus_label(k) != 'Controller':
        raise StepFailed(f'after the settings the highlight should be back on CONTROLLER, not "{focus_label(k)}"')
    tap(k, 'B')
    wait(lambda: button_center(k, 'Controller') is None, 3, 'B did not close Options')
    time.sleep(1)
    ok('controller: menus, Options > CONTROLLER, remapping, B to go back')


def last_address():
    """The address Join Game remembers (both copies share the game folder's mods\\netplay.ini), or ''."""
    try:
        with open(os.path.join(GAME, 'mods', 'netplay.ini')) as f:
            for line in f:
                if line.startswith('last_address='):
                    return line.split('=', 1)[1].strip()
    except OSError:
        pass
    return ''


def pad_connect():
    """Duel > Online Play > Host Game / Join Game; B types the address on the on-screen keyboard."""
    send('B', 'setname Dad')
    since = mark()
    for k in 'AB':
        pad(k)
        time.sleep(0.2)
        pad_press(k, 676, 371, 'Duel')
    for k in 'AB':
        wait(lambda: dialog_open(k, DLG_MODE), 5, f'copy {k}: Duel did not open the Local/Online dialog')
    time.sleep(1.2)
    for k in 'AB':
        pad_press_label(k, 'ONLINE PLAY')
    for k in 'AB':
        wait(lambda: dialog_open(k, DLG_ROLE), 5, f'copy {k}: Online Play did not open Host/Join')
    time.sleep(1.2)
    pad_press_label('A', 'HOST GAME')
    wait_log('A', 'net: listening', since, 5)
    pad_press_label('B', 'JOIN GAME')
    wait(lambda: dialog_open('B', DLG_ADDRESS), 5, 'copy B: Join Game did not open the address box')
    time.sleep(1.2)
    if last_address():                                       # the box comes filled in: OK first
        if focus_label('B') != 'OK':
            raise StepFailed(f'the address box (filled in) should start on OK, not "{focus_label("B")}"')
        tap('B', 'UP')
    if not pad_state('B')['focus'].startswith('EditWidget'):
        raise StepFailed(f'the highlight should be on the address box (from OK, or empty), not {pad_state("B")["focus"]}')
    tap('B', 'A')
    wait(lambda: pad_state('B')['context'] == 'keyboard', 3, 'A on the address box did not open the keyboard')
    for _ in range(20):
        tap('B', 'B', hold=0.06)                             # B is backspace on the keyboard
    if pad_state('B').get('text'):
        raise StepFailed(f'backspace did not empty the box: {pad_state("B").get("text")}')
    osk_type('B', '127.0.0.1')
    if pad_state('B').get('text') != '127.0.0.1':
        raise StepFailed(f'the keyboard typed {pad_state("B").get("text")!r}, not 127.0.0.1')
    frame('B', 'pad_keyboard')
    tap('B', 'START')
    wait(lambda: pad_state('B')['context'] == 'menu' and focus_label('B') == 'OK', 3,
         'START did not put the keyboard away onto OK')
    tap('B', 'A')
    for k in 'AB':
        wait_log(k, 'online: opponent is', since, 10)
    ok('controller: connected (A hosted, B typed 127.0.0.1 on the on-screen keyboard)')


def pad_level():
    time.sleep(1.5)                                          # the level screen slides in on A
    if focus_label('A') != 'Level 1':
        tap('A', 'DOWN')
    since = mark()
    if focus_label('A') != 'Level 1':
        raise StepFailed(f'the level screen should start on the first level, not "{focus_label("A")}"')
    tap('A', 'A')
    for k in 'AB':
        wait_log(k, 'lockstep: begin', since, 10)
    wait(lambda: all(state(k).get('seldlg') == '1' for k in 'AB'), 15, 'the master selection never appeared on both')
    ok('controller: A picked the level')


def pad_duel():
    time.sleep(2.5)                                          # the selection slides in
    since = mark()
    pad_press('A', 271, 391, 'the crocodile')
    pad_press('B', 600, 173, 'Jimmy Lightning')
    for _ in range(4):                                       # the joiner never reaches Play Now (the host's)
        tap('B', 'DOWN')
        if focus_label('B').startswith('Play Now'):
            raise StepFailed("the joiner's highlight reached Play Now")
    time.sleep(0.5)
    pad_press('A', 399, 547, 'Play Now')
    for k in 'AB':
        wait_log(k, 'duel started', since, 10)
    ok('controller: masters picked, A pressed Play Now')


def aiming():
    return next((k for k in 'AB' if state(k).get('control') == '1' and state(k).get('state') == '1'), None)


def pad_shoot(rnd):
    """Whoever is aiming turns the cannon with both sticks and shoots with A; the other one's controller does
    nothing to the shared game."""
    k = wait(aiming, 40, 'no one gets to shoot')
    other = 'B' if k == 'A' else 'A'
    a0 = pad_state(k)['aim']
    pad(other, '-', 1.0, 0)                                  # the opponent pushes their stick: nothing happens
    time.sleep(0.4)
    pad(other)
    time.sleep(0.3)
    if pad_state(k)['aim'] != a0:
        raise StepFailed(f'copy {other} turned the cannon while copy {k} was aiming')
    first = rnd.choice([-1, 1])
    for x in (first, -first):                                # the other way if the cannon is at its limit
        pad(k, '-', x * rnd.uniform(0.5, 1.0), 0)            # left stick
        time.sleep(rnd.uniform(0.15, 0.4))
        pad(k, '-', 0, 0, rnd.uniform(-1, 1), 0)             # right stick, fine
        time.sleep(0.3)
        pad(k)
        time.sleep(0.2)
        if pad_state(k)['aim'] != a0:
            break
    else:
        raise StepFailed(f'copy {k}: the sticks did not turn the cannon')
    balls = state(k)['balls']
    tap(k, 'A')
    wait(lambda: state(k)['balls'] != balls, 5, f'copy {k}: A did not shoot')
    print(f'  copy {k} aimed ({a0} -> {pad_state(k)["aim"]}) and shot', flush=True)


def pad_chat():
    """The player aiming chats with Y, the D-pad and A: the other sees it; closing chat doesn't shoot or aim."""
    wait(aiming, 40, 'no one is aiming')
    k = aiming()
    other = 'B' if k == 'A' else 'A'
    aim, balls = pad_state(k)['aim'], state(k)['balls']
    tap(k, 'Y')
    wait(lambda: chat_state(k)['open'] == '1', 3, f'copy {k}: Y did not open the phrases')
    for _ in range(3):
        tap(k, 'DOWN')
    tap(k, 'A')
    wait(lambda: chat_state(other)['last'].endswith(': Lucky!'), 5, f'copy {other} never showed "Lucky!"')
    tap(k, 'Y')
    tap(k, 'B')
    wait(lambda: chat_state(k)['open'] == '0', 3, f'copy {k}: B did not close the phrases')
    time.sleep(1)
    if (state(k)['state'], state(k)['balls']) != ('1', balls):
        raise StepFailed(f'copy {k}: chatting shot the ball')
    if pad_state(k)['aim'] != aim:
        raise StepFailed(f'copy {k}: chatting turned the cannon')
    tap(k, 'START')
    wait(lambda: dialog_open(k, DLG_GAME_MENU), 5, f'copy {k}: START did not open the Menu')
    time.sleep(1.2)
    tap(k, 'START')
    wait(lambda: not dialog_open(k, DLG_GAME_MENU), 5, f'copy {k}: START did not close the Menu')
    ok(f'controller: copy {k} chatted, opened and closed the Menu, no stray shot')


def pad_results():
    """The joiner's highlight only reaches Main Menu; the host replays the level."""
    pad('B')
    labels = set()
    for b in ['DOWN', 'UP', 'LEFT', 'RIGHT', 'DOWN', 'RIGHT', 'UP']:
        tap('B', b)
        labels.add(focus_label('B'))
    if labels - {'Main Menu', ''}:
        raise StepFailed(f"the joiner's highlight reached {labels - {'Main Menu', ''}} on the results")
    since = mark()
    pad_press_label('A', 'Replay Level')
    wait_log('A', 'results button 1001 pressed (host decides)', since, 5)
    wait(lambda: all(state(k).get('seldlg') == '1' for k in 'AB'), 20, 'Replay Level did not bring back the selection')
    ok('controller: the host replayed the level from the results')


def pad_leave(k):
    """Menu > Main Menu > Leave, with the controller; "Leave Game?" starts on Cancel."""
    since = mark()
    tap(k, 'START')
    wait(lambda: dialog_open(k, DLG_GAME_MENU), 5, f'copy {k}: START did not open the Menu')
    time.sleep(1.2)
    pad_press_label(k, 'Main Menu')
    wait(lambda: dialog_open(k, DLG_LEAVE_GAME) or not dialog_open(k, DLG_GAME_MENU), 5,
         f'copy {k}: Main Menu did nothing')
    if dialog_open(k, DLG_LEAVE_GAME):                       # the game doesn't always ask
        time.sleep(1.2)
        if focus_label(k).upper() != 'CANCEL':
            raise StepFailed(f'"Leave Game?" should start on CANCEL, not "{focus_label(k)}"')
        tap(k, 'LEFT')
        tap(k, 'A')
    wait_log(k, 'leaving session', since, 10)
    wait_log('B' if k == 'A' else 'A', 'left the game', since, 10, f'the other copy was not told that {k} left')
    ok(f'controller: copy {k} left the duel')


def padscenario():
    rnd = random.Random(5)
    pad_menus()
    pad_connect()
    pad_level()
    pad_duel()
    wait(aiming, 40, 'after Play Now no one gets to shoot')
    for _ in range(3):
        pad_shoot(rnd)
    pad_chat()
    time.sleep(4)
    synced('pad_play')
    if autoplay(40, seed=11) != 'results':                  # the rest of the duel, quickly, by mouse
        raise StepFailed('the duel did not reach the results screen')
    results_settled()
    pad_results()
    pad_duel()
    pad_shoot(rnd)
    time.sleep(6)
    synced('pad_replay')
    pad_leave('B')
    for k in 'AB':
        send(k, 'pad off')
    ok('controller scenario passed')


# ---- a local duel with two controllers (copy A) ----

def board(k):
    """The board in play, online or not: state, player (whose turn), balls, steps (logic steps run so far), replay
    (an Instant Replay is on), picked (the masters chosen on the selection)."""
    return dict(kv.split('=') for kv in send(k, 'board').split() if '=' in kv)


def step_rate(k, seconds=1.0):
    """Logic steps a second: about 100, or 8 times that fast-forwarded."""
    first, t = int(board(k)['steps']), time.time()
    time.sleep(seconds)
    return (int(board(k)['steps']) - first) / (time.time() - t)


def frame_at(k, p):
    """The center of player p's (1 or 2) highlight frame, or None."""
    f = pad_state(k)[f'frame{p}']
    return None if f == 'none' else tuple(map(int, f.split(',')))


def player_goto(k, p, x, y, what, steps=14):
    """Move player p's own highlight on the selection, with their controller, onto the target at (x, y)."""
    for _ in range(steps):
        at = frame_at(k, p)
        if at is None:
            raise StepFailed(f'player {p} has no highlight on the selection')
        if abs(at[0] - x) <= 6 and abs(at[1] - y) <= 6:
            return
        dx, dy = x - at[0], y - at[1]
        tap(k, ('RIGHT' if dx > 0 else 'LEFT') if abs(dx) > abs(dy) else ('DOWN' if dy > 0 else 'UP'), n=p)
    raise StepFailed(f'player {p} could not reach {what} (highlight at {frame_at(k, p)})')


def turn_controller(k):
    """Whose turn it is (0, 1), and the number of the controller that plays it."""
    p = int(board(k)['player'])
    return p, (p ^ (pad_state(k).get('swapped') == '1')) + 1


def local_start(k='A'):
    """Two controllers; controller 1 goes Duel > Local Play > the first level."""
    pad(k)
    pad(k, n=2)
    time.sleep(0.3)
    if pad_state(k).get('count') != '2':
        raise StepFailed(f'the second pretend controller did not connect: {send(k, "padstate")}')
    pad_press(k, 676, 371, 'Duel')
    wait(lambda: dialog_open(k, DLG_MODE), 5, 'Duel did not open the Local/Online dialog')
    time.sleep(1.2)
    pad_press_label(k, 'LOCAL PLAY')
    time.sleep(1.5)
    if dialog_open(k, DLG_CONTINUE_GAME):                    # an earlier local duel was left and saved
        time.sleep(1.2)
        pad_press_label(k, next((s for s in ('New Game', 'NEW GAME') if button_center(k, s)), 'New Game'))
        wait(lambda: not dialog_open(k, DLG_CONTINUE_GAME), 5, 'New Game did not close "Continue Game?"')
    time.sleep(2.5)                                          # the level screen slides in
    if focus_label(k) != 'Level 1':
        tap(k, 'DOWN')
    if focus_label(k) != 'Level 1':
        raise StepFailed(f'the level screen should start on the first level, not "{focus_label(k)}"')
    tap(k, 'A')
    wait(lambda: dialog_open(k, DLG_SELECTION), 10, 'the local duel never reached the master selection')
    time.sleep(2)                                            # slides in
    wait(lambda: pad_state(k)['context'] == 'players', 3,
         f'the selection did not give each player a highlight: {send(k, "padstate")}')
    ok('local duel: two controllers, each player has a highlight on the selection')


def local_select(k='A'):
    """Each player moves their own highlight with their own controller, only on their side, and picks a master;
    player 2 presses Play Now."""
    f1, f2 = frame_at(k, 1), frame_at(k, 2)
    if not f1 or not f2 or f1[0] >= 400 or f2[0] <= 400:
        raise StepFailed(f'the highlights should start on each side: player 1 {f1}, player 2 {f2}')
    player_goto(k, 1, 400, 201, 'Play Computer')             # one person: one shared highlight again
    tap(k, 'A', n=1)
    wait(lambda: pad_state(k)['split'] == '0' and pad_state(k)['context'] == 'menu', 3,
         'Play Computer did not end the two highlights')
    pad_goto(k, 400, 167, 'Play a Friend')
    tap(k, 'A')
    wait(lambda: pad_state(k)['context'] == 'players', 3, 'Play a Friend did not bring back the two highlights')
    f1, f2 = frame_at(k, 1), frame_at(k, 2)
    tap(k, 'DOWN', n=2)
    if frame_at(k, 1) != f1 or frame_at(k, 2) == f2:
        raise StepFailed(f"controller 2 should move only player 2's highlight: {f1} -> {frame_at(k, 1)}, "
                         f"{f2} -> {frame_at(k, 2)}")
    for _ in range(4):
        tap(k, 'LEFT', n=2)
        if frame_at(k, 2)[0] < 399:
            raise StepFailed(f"player 2's highlight reached player 1's side: {frame_at(k, 2)}")
    for _ in range(4):
        tap(k, 'RIGHT', n=1)
        if frame_at(k, 1)[0] > 465:
            raise StepFailed(f"player 1's highlight reached player 2's side: {frame_at(k, 1)}")
    player_goto(k, 2, 601, 176, "player 2's Jimmy Lightning")
    tap(k, 'A', n=2)
    player_goto(k, 1, 271, 392, "player 1's dragon")
    tap(k, 'A', n=1)
    time.sleep(0.3)
    if board(k)['picked'] != '7,1':
        raise StepFailed(f"the masters picked should be 7 and 1, not {board(k)['picked']}")
    frame(k, 'local_select')
    player_goto(k, 2, 399, 547, 'Play Now')
    tap(k, 'A', n=2)
    wait(lambda: not dialog_open(k, DLG_SELECTION), 5, "player 2's Play Now did not start the duel")
    wait(lambda: board(k)['state'] == '1', 20, 'the local duel never got to the first shot')
    ok('local duel: each player picked with their own controller, player 2 pressed Play Now')


def local_turn(k='A', new_turn=True):
    """Whoever's turn it is plays it with their own controller (aims, fast-forwards, shoots, feels it); the other
    controller does nothing to it. Then fast-forwards to the next turn."""
    wait(lambda: board(k)['state'] == '1', 30, 'no one gets to shoot')
    p, mine = turn_controller(k)
    other = 3 - mine
    s = pad_state(k)
    if new_turn and s['buzzed'] != str(mine - 1):
        raise StepFailed(f"player {p + 1}'s turn buzzed controller {int(s['buzzed']) + 1}, not {mine}")
    a0, balls = s['aim'], board(k)['balls']
    pad(k, '-', 1.0, 0, n=other)
    time.sleep(0.4)
    pad(k, 'A', n=other)
    time.sleep(0.15)
    pad(k, n=other)
    time.sleep(0.5)
    if pad_state(k)['aim'] != a0 or board(k)['balls'] != balls:
        raise StepFailed(f"controller {other} aimed or shot on player {p + 1}'s turn")
    for b in ('START', 'X'):                                 # nor opens the Menu or an Instant Replay
        tap(k, b, n=other)
        time.sleep(0.4)
        if dialog_open(k, DLG_GAME_MENU) or board(k)['replay'] != '0':
            raise StepFailed(f"controller {other}'s {b} opened something on player {p + 1}'s turn")
    pad(k, 'RT', n=other)
    slow = step_rate(k)
    pad(k, n=other)
    pad(k, 'RT', n=mine)
    fast = step_rate(k)
    pad(k, n=mine)
    if slow > 200 or fast < 500:
        raise StepFailed(f'fast-forward: controller {other} ran {slow:.0f} steps/s, controller {mine} {fast:.0f} '
                         f'(want about 100 and 800)')
    for x in (0.8, -0.8):                                    # the other way if the cannon is at its limit
        pad(k, '-', x, 0, n=mine)
        time.sleep(0.3)
        pad(k, n=mine)
        time.sleep(0.2)
        if pad_state(k)['aim'] != a0:
            break
    else:
        raise StepFailed(f'controller {mine} did not turn the cannon')
    tap(k, 'A', n=mine)
    wait(lambda: board(k)['balls'] != balls, 5, f'controller {mine} did not shoot')
    time.sleep(0.3)
    if pad_state(k)['rumbleto'] != str(mine - 1):
        raise StepFailed(f"the shot's vibration went to controller {int(pad_state(k)['rumbleto']) + 1}, not {mine}")
    pad(k, 'RT', n=mine)                                     # through to the next turn
    wait(lambda: board(k)['state'] == '1' and board(k)['player'] != str(p), 40, 'the turn never passed')
    pad(k, n=mine)
    ok(f'player {p + 1}: controller {mine} aimed, fast-forwarded ({fast:.0f} steps/s) and shot; '
       f'controller {other} did nothing ({slow:.0f} steps/s)')


REPLAY_AGAIN_AT, REPLAY_DONE_AT = '761,420', '761,564'  # the panel's Instant Replay and Done buttons


def local_replay(k='A'):
    """X shows the Instant Replay; its panel's highlight (the pointer, lighting the button up) starts on Done, RT
    fast-forwards (the pointer goes onto the board meanwhile), A on Instant Replay watches again, B is Done."""
    wait(lambda: board(k)['state'] == '1', 30, 'no one is aiming')
    p, mine = turn_controller(k)
    tap(k, 'X', n=mine)
    wait(lambda: pad_state(k)['context'] == 'replay', 5, 'X did not start the Instant Replay')
    time.sleep(0.3)
    s = pad_state(k)
    if s['replayfocus'] != '4' or s['pointer'] != REPLAY_DONE_AT:
        raise StepFailed(f"the replay panel should start on Done, not button {s['replayfocus']} (pointer {s['pointer']})")
    pad(k, 'RT', n=mine)
    rate = step_rate(k)
    wait(lambda: board(k)['state'] == '5', 30, 'the replay never finished')
    pad(k, n=mine)
    if rate < 300:
        raise StepFailed(f'RT should fast-forward the replay (about 400 steps/s), not {rate:.0f}')
    for _ in range(2):
        tap(k, 'UP', n=mine)
    s = pad_state(k)
    if s['replayfocus'] != '0' or s['pointer'] != REPLAY_AGAIN_AT:
        raise StepFailed(f"two UPs should reach Instant Replay, not button {s['replayfocus']} (pointer {s['pointer']})")
    frame(k, 'local_replay')
    tap(k, 'A', n=mine)
    wait(lambda: board(k)['state'] == '2', 5, 'Instant Replay on the panel did not play it again')
    tap(k, 'B', n=mine)
    wait(lambda: pad_state(k)['context'] == 'board' and board(k)['replay'] == '0', 5, 'B did not end the replay')
    wait(lambda: board(k)['state'] == '1', 5, 'after the replay the turn did not come back')
    ok(f'controller {mine}: Instant Replay, fast-forwarded ({rate:.0f} steps/s), watched again, Done')


def frame_wait_ms(k, n=6):
    """How long the game takes to get round to the mod (the test pipe, like the controller, is served once a
    frame): milliseconds, the slowest of n tries."""
    worst = 0
    for _ in range(n):
        t = time.time()
        send(k, 'ping')
        worst = max(worst, (time.time() - t) * 1000)
        time.sleep(0.137)
    return worst


def local_swap(k='A', swapped=True):
    """The player whose turn it is opens the Menu (START) > CONTROLLER and swaps the players' controllers."""
    wait(lambda: board(k)['state'] == '1', 30, 'no one is aiming')
    p, mine = turn_controller(k)
    tap(k, 'START', n=mine)
    wait(lambda: dialog_open(k, DLG_GAME_MENU), 5, f'START on controller {mine} did not open the Menu')
    time.sleep(1.2)
    ms = frame_wait_ms(k)                                    # a still screen: the game redraws once a second
    if ms > 150:
        raise StepFailed(f'with the Menu over the paused board the mod runs only every {ms:.0f} ms '
                         '(a quick controller tap would be missed)')
    borderless_box(k)                                        # the in-game Menu has the BORDERLESS row too
    pad_press_label(k, 'Controller')
    wait(lambda: pad_state(k)['dialog'] == '1', 5, 'CONTROLLER did not open the controller settings')
    time.sleep(1.2)
    before, after = ('1', '2') if swapped else ('2', '1')
    pad_press_label(k, f'LOCAL DUEL: CONTROLLER 1 = PLAYER {before}')
    wait(lambda: pad_state(k)['swapped'] == ('1' if swapped else '0'), 3, 'the LOCAL DUEL row did not swap')
    if not button_center(k, f'LOCAL DUEL: CONTROLLER 1 = PLAYER {after}'):
        raise StepFailed('the LOCAL DUEL row did not change its lettering')
    frame(k, 'local_swap')
    tap(k, 'B')
    wait(lambda: pad_state(k)['dialog'] == '0', 3, 'B did not close the controller settings')
    time.sleep(1)
    tap(k, 'START')
    wait(lambda: not dialog_open(k, DLG_GAME_MENU), 5, 'START did not close the Menu')
    time.sleep(1)
    ok(f"controllers {'swapped' if swapped else 'back'}: controller 1 plays player {after}")


def local_one_controller(k='A'):
    """With controller 2 unplugged, controller 1 plays both players' turns, as one mouse does."""
    send(k, 'pad 2 off')
    time.sleep(0.3)
    s = pad_state(k)
    if s['count'] != '1' or s['split'] != '0':
        raise StepFailed(f"one controller left should mean no split: count={s['count']} split={s['split']}")
    for _ in range(2):                                       # until it has played player 2's turn
        wait(lambda: board(k)['state'] == '1', 40, 'no one gets to shoot')
        p, balls = int(board(k)['player']), board(k)['balls']
        tap(k, 'A')
        wait(lambda: board(k)['balls'] != balls, 5, f"controller 1 did not shoot on player {p + 1}'s turn")
        pad(k, 'RT')
        wait(lambda: board(k)['state'] == '1' and board(k)['player'] != str(p), 40, 'the turn never passed')
        pad(k)
        if p == 1:
            break
    ok("one controller: it played player 2's turn too")


def local_leave(k='A'):
    """Menu > Main Menu > Leave, by controller, back to the main menu."""
    wait(lambda: board(k)['state'] == '1', 30, 'no one is aiming')
    tap(k, 'START')
    wait(lambda: dialog_open(k, DLG_GAME_MENU), 5, 'START did not open the Menu')
    time.sleep(1.2)
    pad_press_label(k, 'Main Menu')
    wait(lambda: dialog_open(k, DLG_LEAVE_GAME) or not dialog_open(k, DLG_GAME_MENU), 5, 'Main Menu did nothing')
    if dialog_open(k, DLG_LEAVE_GAME):
        time.sleep(1.2)
        tap(k, 'LEFT')
        tap(k, 'A')
    wait(lambda: button_center(k, 'Options'), 10, 'leaving the local duel did not get back to the main menu')
    ok('left the local duel')


def pad_replays(k='A'):
    """Main menu > Replays by controller: a saved replay plays with the replay controls (Done lit, B is Done),
    and B leaves the list again (its Cancel says Main Menu after a replay). Skipped without saved replays."""
    pad(k)
    time.sleep(0.3)
    pad_press_label(k, 'Replays')
    time.sleep(2)
    if button_center(k, 'Ok') and not button_center(k, 'Play'):  # a note about saving replays comes first
        tap(k, 'A')
        time.sleep(2)
    if not button_center(k, 'Play'):
        raise StepFailed('Replays did not show the list of saved replays')
    if not any('ListWidget' in line for line in send(k, 'widgets').splitlines()) or 'Play' not in focus_label(k):
        tap(k, 'B')
        ok(f'Replays: no saved replay to play (highlight on "{focus_label(k)}"), skipped')
        return
    tap(k, 'A')
    wait(lambda: pad_state(k)['context'] == 'replay', 10, 'Play did not show the saved replay')
    time.sleep(0.3)
    s = pad_state(k)
    if s['replayfocus'] != '4' or s['pointer'] != REPLAY_DONE_AT:
        raise StepFailed(f"the replay should start on Done, not button {s['replayfocus']} (pointer {s['pointer']})")
    tap(k, 'B')
    wait(lambda: button_center(k, 'Play'), 10, 'B (Done) did not go back to the list of replays')
    time.sleep(1.5)
    tap(k, 'B')
    wait(lambda: not button_center(k, 'Play') and button_center(k, 'Options'), 10, 'B did not leave the replays')
    ok('controller: a saved replay played from Replays; B was Done, then left the list')


# ---- the display: a window scaled to the monitor's text size, or borderless across the monitor (copy A) ----

def display_state(k):
    """mode (windowed, borderless, fullscreen), dpi, window, client (WxH), picture (x,y,w,h in the client),
    remapfrom (the mouse mapping's window rect), checkbox (BORDERLESS in Options: 1, 0, or -1 when closed),
    present (copy, direct3d, gdi)."""
    return dict(kv.split('=', 1) for kv in send(k, 'display').split() if '=' in kv)


def _user32():
    import ctypes
    user = ctypes.windll.user32
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))  # physical pixels, whatever Python itself declared
    return user


def monitors():
    """Every monitor's rect in physical pixels."""
    import ctypes
    from ctypes import wintypes
    user = _user32()
    found = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC, ctypes.POINTER(wintypes.RECT),
                              wintypes.LPARAM)

    def each(h, dc, r, lp):
        found.append((r.contents.left, r.contents.top, r.contents.right, r.contents.bottom))
        return True
    user.EnumDisplayMonitors(None, None, proc(each), 0)
    return found


def move_window(k, x, y):
    _user32().SetWindowPos(load()[k]['hwnd'], None, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010)  # no size/z-order/activate


def window_rect(k):
    import ctypes
    from ctypes import wintypes
    r = wintypes.RECT()
    _user32().GetWindowRect(load()[k]['hwnd'], ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def screen_picture(k):
    """What copy k's client area shows on the screen, and the game's own frame at the same moment."""
    import ctypes
    from ctypes import wintypes
    from PIL import ImageGrab
    user = _user32()
    hwnd = load()[k]['hwnd']
    c = wintypes.RECT()
    user.GetClientRect(hwnd, ctypes.byref(c))
    p = wintypes.POINT(0, 0)
    user.ClientToScreen(hwnd, ctypes.byref(p))
    shown = ImageGrab.grab(bbox=(p.x, p.y, p.x + c.right, p.y + c.bottom), all_screens=True).convert('RGB')
    return shown, frame(k, 'display_game')


def picture_matches(k, region=(0.0, 1.0), name='display'):
    """The picture copy k shows (the given horizontal fraction of it: other test windows may overlap) is its game
    frame, scaled; returns the shown image."""
    from PIL import Image, ImageChops, ImageStat
    shown, game = screen_picture(k)
    shown.save(os.path.join(SHOTS, f'{name}_screen_{k}.png'))
    px, py, pw, ph = map(int, display_state(k)['picture'].split(','))
    a, b = region
    picture = shown.crop((px + int(pw * a), py, px + int(pw * b), py + ph))
    expect = game.crop((int(800 * a), 0, int(800 * b), 600)).resize(picture.size, Image.BILINEAR)
    diff = ImageStat.Stat(ImageChops.difference(picture, expect).convert('L')).mean[0]
    if diff > 12:
        raise StepFailed(f'copy {k}: the screen does not show the game ({name}: mean difference {diff:.1f}, see '
                         f'{name}_screen_{k}.png)')
    return shown, diff


def options_checkboxes(k):
    """The visible checkboxes as (x, y, w, h), in the game's 800x600."""
    out = []
    for line in send(k, 'widgets').splitlines():
        s = line.strip()
        if s.startswith('ThunderCheckbox') and ' vm' in s:
            x, y = map(int, s.split()[1].split(','))
            w, h = map(int, s.split()[2].split('x'))
            out.append((x, y, w, h))
    return out


def borderless_box(k):
    """BORDERLESS: the checkbox under the left column (the lowest of them)."""
    boxes = options_checkboxes(k)
    if len(boxes) != 5:
        raise StepFailed(f'copy {k}: Options should have 5 checkboxes with BORDERLESS, not {len(boxes)}')
    x, y, w, h = max(boxes, key=lambda b: (b[1], -b[0]))
    left = sorted(b for b in boxes if b[0] == x)
    if len(left) != 3 or y - left[1][1] != left[1][1] - left[0][1]:
        raise StepFailed(f'copy {k}: BORDERLESS is not a row under Fullscreen and Custom Cursors: {boxes}')
    return x + w // 2, y + h // 2


def display_windowed(k='A'):
    """In a window the picture is as big as the monitor's text scaling, drawn by the mod (sharp), mouse mapped."""
    d = display_state(k)
    dpi = int(d['dpi'])
    want = f'{800 * dpi // 96}x{600 * dpi // 96}'
    if d['mode'] != 'windowed' or d['client'] != want or d['picture'] != f'0,0,{want.replace("x", ",")}':
        raise StepFailed(f'copy {k}: the window should be {want} at {dpi} dpi: {send(k, "display")}')
    if d['present'] != ('copy' if dpi == 96 else 'direct3d'):
        raise StepFailed(f'copy {k}: frames go out by {d["present"]}, not by {"copy" if dpi == 96 else "direct3d"}')
    _, diff = picture_matches(k, (0.0, 0.6), 'windowed')
    ok(f'copy {k}: window {want} at {dpi} dpi, drawn by {d["present"]} (difference {diff:.1f})')


def mouse_mapped(k):
    """Windows' mouse positions in the window reach the game as the matching point of its 800x600."""
    px, py, pw, ph = map(int, display_state(k)['picture'].split(','))
    for fx, fy in ((0.5, 0.5), (0.25, 0.75), (0.9, 0.1)):
        send(k, f'winmouse {px + int(pw * fx)} {py + int(ph * fy)}')
        time.sleep(0.3)
        got = tuple(map(int, pad_state(k)['pointer'].split(',')))
        if abs(got[0] - 800 * fx) > 2 or abs(got[1] - 600 * fy) > 2:
            raise StepFailed(f'copy {k}: the mouse at {fx:.2f},{fy:.2f} of the picture reached the game at {got}')


def mouse_held(k, spots=((0.5, 0.5), (0.97, 0.5), (0.5, 0.97), (0.98, 0.98), (0.03, 0.03))):
    """With the mouse anywhere over the picture (a pretend position: what the game reads from Windows, so the real
    mouse stays put), the game keeps it in its window. Its check every 10 ms measured 800x600 from the window's corner,
    so on a bigger picture it let go near the right and bottom between mouse messages: the pointer flickered and
    clicks were lost. The cursor thread's point (where the custom cursor goes) is the game's point too. Spots are
    fractions of the picture; beyond 1 is the black bar (borderless), where the cursor stays at the picture's edge."""
    d = display_state(k)
    if d['under'] == 'picture':                              # (the real WindowFromPoint, on the game's thread)
        raise StepFailed(f"copy {k}: the game's mouse check finds the mod's picture window over the game's")
    px, py, pw, ph = map(int, d['picture'].split(','))
    try:
        for fx, fy in spots:
            x, y = px + int(pw * fx), py + int(ph * fy)
            send(k, f'cursor {x} {y}')
            send(k, f'winmouse {x} {y}')                     # the mouse message that comes with it
            time.sleep(0.3)
            for _ in range(4):
                d = display_state(k)
                if d['mousein'] != '1':
                    raise StepFailed(f'copy {k}: the game let go of the mouse at {fx:.2f},{fy:.2f} of the picture '
                                     f'({x},{y} in the window): {d}')
                time.sleep(0.1)
            want = (min(int(800 * fx), 799), min(int(600 * fy), 599))
            got = tuple(map(int, d['cursor'].split(',')))
            if abs(got[0] - want[0]) > 2 or abs(got[1] - want[1]) > 2:
                raise StepFailed(f'copy {k}: the cursor thread has the mouse at {got} of the frame, not {want}')
    finally:
        send(k, 'cursor off')


def custom_cursor(k='A'):
    """Options > Custom Cursors: the game draws its own pointer into the frame where the mouse is (mapped), and what
    the screen shows is still the frame (the pointer drawn straight onto the screen would be in the wrong place)."""
    from PIL import ImageChops, ImageStat

    def set_custom_cursors(want):                            # Options > Custom Cursors, which Done applies
        click(k, 313, 571)
        wait(lambda: len(options_checkboxes(k)) == 5, 5, 'Options did not open')
        time.sleep(1.5)
        boxes = options_checkboxes(k)
        lx = min(b[0] for b in boxes)
        x, y, w, h = sorted(b for b in boxes if b[0] == lx)[1]  # the left column: Fullscreen, Custom Cursors, ...
        click(k, x + w // 2, y + h // 2)
        time.sleep(0.5)
        click(k, *button_center(k, 'Done'))
        wait(lambda: on() == want, 5, f'Custom Cursors did not turn {"on" if want else "off"}')
        time.sleep(1.5)

    on = lambda: int(send(k, 'peek app 500 1')) & 0xFF      # noqa: E731
    was = on()
    if not was:
        set_custom_cursors(1)
    try:
        px, py, pw, ph = map(int, display_state(k)['picture'].split(','))
        spots = [(px + gx * pw // 800, py + gy * ph // 600) for gx, gy in ((120, 100), (400, 100))]  # still parts
        shown = []
        for i, (cx, cy) in enumerate(spots):
            send(k, f'cursor {cx} {cy}')
            send(k, f'winmouse {cx} {cy}')
            time.sleep(0.8)
            shown.append(screen_picture(k)[0])               # (the game adds its pointer after the frame we capture)
            shown[-1].save(os.path.join(SHOTS, f'custom_cursor_{i}_{k}.png'))
        reach = pw * 30 // 800
        for cx, cy in spots:
            box = (cx - reach, cy - reach, cx + reach, cy + reach)
            moved = ImageStat.Stat(ImageChops.difference(shown[0].crop(box), shown[1].crop(box))).mean
            if max(moved) < 4:
                raise StepFailed(f'copy {k}: with Custom Cursors no pointer showed at {cx},{cy} of the window '
                                 f'(see custom_cursor_*_{k}.png)')
        _, diff = picture_matches(k, (0.0, 0.6), 'custom_cursor')
        ok(f'copy {k}: Custom Cursors drawn where the mouse is in the frame; the screen shows the frame '
           f'(difference {diff:.1f}, {display_state(k)["partial"]} pointer drawings onto the screen left out)')
    finally:
        send(k, 'cursor off')
        if not was:
            set_custom_cursors(0)


def display_borderless(k='A'):
    """Options > BORDERLESS: the window covers its monitor, the picture fills its height in shape with black bars,
    the mouse maps; Fullscreen unticks it; it survives Done; unticking puts the window back."""
    mons = monitors()
    m = min(mons, key=lambda r: ((r[2] - r[0]) * (r[3] - r[1]), r[0] == 0 and r[1] == 0))  # smallest, not primary
    home = window_rect(k)
    move_window(k, m[0] + 40, m[1] + 40)
    time.sleep(1.5)
    display_windowed(k)                                      # on that monitor's scaling
    click(k, 313, 571)                                       # Options
    wait(lambda: len(options_checkboxes(k)) == 5, 5, 'Options did not open with BORDERLESS')
    time.sleep(1.5)
    frame(k, 'display_options')
    click(k, *borderless_box(k))
    wait(lambda: display_state(k)['mode'] == 'borderless', 5, 'BORDERLESS did not make the window borderless')
    time.sleep(1)
    d = display_state(k)
    mw, mh = m[2] - m[0], m[3] - m[1]
    ph = mh if mw * 3 >= mh * 4 else mw * 3 // 4
    pw = ph * 4 // 3
    if d['window'] != ','.join(map(str, m)) or d['client'] != f'{mw}x{mh}' or \
            d['picture'] != f'{(mw - pw) // 2},{(mh - ph) // 2},{pw},{ph}' or d['remapfrom'] != d['picture']:
        raise StepFailed(f'copy {k}: borderless should cover {m} with a {pw}x{ph} picture: {send(k, "display")}')
    mouse_mapped(k)
    spots = [(0.5, 0.5), (0.97, 0.97), (0.03, 0.97)]
    if mw > pw:
        spots.append((1.0 + (mw - pw) / 4 / pw, 0.5))         # over the black bar on the right
    mouse_held(k, spots)
    shown, diff = picture_matches(k, (0.0, 1.0), 'borderless')
    from PIL import ImageStat
    bar = shown.crop(((mw + pw) // 2 + 2, 0, mw, mh)).convert('L')  # the right one (others may overlap the left)
    if bar.width > 0 and ImageStat.Stat(bar).extrema[0][1] > 8:
        raise StepFailed(f'copy {k}: the bar right of the picture is not black (see borderless_screen_{k}.png)')
    ok(f'copy {k}: borderless across {mw}x{mh}, picture {pw}x{ph}, mouse mapped, bars black (difference {diff:.1f})')
    x, y, w, h = min(options_checkboxes(k), key=lambda b: (b[1], b[0]))
    click(k, x + w // 2, y + h // 2)                         # Fullscreen: unticks BORDERLESS
    wait(lambda: display_state(k)['mode'] == 'windowed' and display_state(k)['checkbox'] == '0', 5,
         'ticking Fullscreen did not untick BORDERLESS')
    click(k, x + w // 2, y + h // 2)                         # Fullscreen off again (Done would switch to it)
    time.sleep(0.5)
    click(k, *borderless_box(k))
    wait(lambda: display_state(k)['mode'] == 'borderless', 5, 'BORDERLESS did not come back')
    click(k, *button_center(k, 'Done'))
    time.sleep(2.5)
    if display_state(k)['mode'] != 'borderless':
        raise StepFailed(f'copy {k}: Done ended borderless: {send(k, "display")}')
    click(k, 313, 571)                                       # Options again: BORDERLESS still ticked
    wait(lambda: display_state(k)['checkbox'] == '1', 5, 'Options did not show BORDERLESS ticked')
    time.sleep(1.5)
    click(k, *borderless_box(k))
    wait(lambda: display_state(k)['mode'] == 'windowed', 5, 'unticking BORDERLESS did not give the window back')
    click(k, *button_center(k, 'Done'))
    time.sleep(2)
    move_window(k, home[0], home[1])
    time.sleep(1)
    display_windowed(k)
    ok(f'copy {k}: Fullscreen unticks BORDERLESS, borderless survives Done, unticking gives the window back')


def displayscenario():
    k = 'A'
    display_windowed(k)
    mouse_held(k)
    ok(f'copy {k}: the mouse stays in over the whole picture; the cursor thread maps it')
    custom_cursor(k)
    display_borderless(k)
    ok('display scenario passed')


def padlocal():
    k = 'A'
    pad_replays(k)
    local_start(k)
    local_select(k)
    local_turn(k)
    local_turn(k)
    local_replay(k)
    local_swap(k, True)
    local_turn(k, new_turn=False)                            # the turn began before the swap
    local_swap(k, False)
    local_one_controller(k)
    local_leave(k)
    send(k, 'pad off')
    ok('local duel with two controllers passed')


# ---- Chaos Mode ----

CHAOS_EFFECTS = ['Big Balls!', 'Pinball Madness!', 'Double Bucket!', 'Teleporting Bucket!', 'Purple Party!',
                 'Peg Shuffle!', 'Invisible Pegs!', 'Blind Shot!', 'Double Shot!', 'Wormholes!', 'Wrap Around!',
                 'Bomb Peg!', 'Lights Out!', 'Upside Down!', 'Portal Storm!', 'Bullet Time!']
CHAOS_BOX_AREA = (330, 400, 470, 490)  # the box's center: the selection's middle column, above Play Now


def chaos_state(k):
    """Chaos Mode on copy k: on (this duel has it), running, round, shots (players who shot this round, bits),
    effect (its number, -1 none), ticks (since it began), announced (how long the announcement has shown, ms),
    box (on the selection: there, ticked, takes clicks), wanted, host (the host's choice, when joining)."""
    return dict(kv.split('=', 1) for kv in send(k, 'chaos').split() if '=' in kv)


def chaos_fx(k):
    """What the effects did on copy k (counters since it started: portal trips, wraps, kicks, second balls, catches,
    slowed, jumps, blind) and where things are (the bomb, the bucket, the cannon, the first ball in play)."""
    return dict(kv.split('=', 1) for kv in send(k, 'chaos fx').split() if '=' in kv)


def chaos_box(k):
    """The CHAOS MODE box on the selection: (center x, center y, ticked), or None."""
    r = send(k, 'chaos box')
    if r == 'none':
        return None
    x, y, w, h, checked = r.split()
    return int(x) + int(w) // 2, int(y) + int(h) // 2, checked == 'checked=1'


def ini_value(section, key):
    import configparser
    cp = configparser.ConfigParser()
    cp.read(os.path.join(GAME, 'mods', 'netplay.ini'))
    return cp.get(section, key, fallback=None)


def chaos_force(k, e):
    send(k, f'chaos now {e}')
    want = str(e) if e != 'none' else '-1'
    name = CHAOS_EFFECTS[e] if e != 'none' else 'no effect'
    wait(lambda: chaos_state(k)['effect'] == want, 10, f'copy {k}: {name} did not begin')


def chaos_shoot(k, x, y):
    """The player in turn on copy k shoots at (x, y) with the mouse, without a Zen Ball: with Master Hu's power the
    game would choose the shot, and its Instant Replay may be a physics step off (re/NOTES.md). (A click just as a turn
    begins can go unheard: it clicks again.) Local duels only (`chaos zen` changes one computer)."""
    wait(lambda: board(k)['state'] == '1', 90, f'copy {k}: no one gets to shoot')
    send(k, 'chaos zen 0')
    balls = board(k)['balls']
    for attempt in range(3):
        send(k, f'move {x} {y}')
        time.sleep(0.3)
        click(k, x, y)
        try:
            wait(lambda: board(k)['balls'] != balls, 2.5, '')
            return
        except StepFailed:
            pass
    raise StepFailed(f'copy {k}: the shot at ({x},{y}) did not go')


def chaos_next_turn(k):
    wait(lambda: board(k)['state'] == '1', 90, f'copy {k}: the next turn never came')


def playfield_brightness(im):
    from PIL import ImageStat
    return ImageStat.Stat(im.crop((90, 140, 710, 560)).convert('L')).mean[0]


def chaos_local_start(k='A', check_box=True):
    """Copy A: Duel > Local Play > the first level. The CHAOS MODE box sits in the selection's middle column, ticks
    and unticks, and netplay.ini remembers it; Play Now with it ticked starts a duel with Chaos Mode."""
    click(k, 680, 372)                                       # Duel
    wait(lambda: dialog_open(k, DLG_MODE), 5, 'Duel did not open the Local/Online dialog')
    time.sleep(0.8)
    click(k, 215, 286)                                       # Local Play
    time.sleep(1.5)
    if dialog_open(k, DLG_CONTINUE_GAME):                    # an earlier local duel was left and saved
        time.sleep(0.8)
        click(k, 440, 297)                                   # New Game
        wait(lambda: not dialog_open(k, DLG_CONTINUE_GAME), 5, 'New Game did not close "Continue Game?"')
    time.sleep(2.5)
    level_page(k, 1)
    click(k, 140, 170)                                       # first level
    wait(lambda: dialog_open(k, DLG_SELECTION), 10, 'the local duel never reached the master selection')
    time.sleep(1.5)                                          # slides in
    box = chaos_box(k)
    if not box:
        raise StepFailed('no CHAOS MODE box on the Two Player Selection')
    x0, y0, x1, y1 = CHAOS_BOX_AREA
    if not (x0 < box[0] < x1 and y0 < box[1] < y1):
        raise StepFailed(f'the CHAOS MODE box is at {box[:2]}, not in the middle column above Play Now')
    if check_box:
        frame(k, 'chaos_selection')
        for want in (not box[2], box[2], True):              # flip it, back, then ticked
            if chaos_box(k)[2] != want:
                click(k, box[0], box[1])
            wait(lambda: chaos_box(k)[2] == want, 3, f'clicking CHAOS MODE did not make it {want}')
            wait(lambda: ini_value('duel', 'chaos') == ('1' if want else '0'), 3,
                 f'netplay.ini did not remember CHAOS MODE {want}')
    elif not box[2]:
        raise StepFailed('CHAOS MODE was not ticked any more (netplay.ini should remember it)')
    click(k, 400, 547)                                       # Play Now
    wait(lambda: not dialog_open(k, DLG_SELECTION), 5, 'Play Now did not start the local duel')
    wait(lambda: board(k)['state'] == '1', 30, 'the duel never got to the first shot')
    s = chaos_state(k)
    if s['on'] != '1' or s['running'] != '1' or s['effect'] != '-1':
        raise StepFailed(f'the duel should have Chaos Mode, running, without an effect yet: {s}')
    ok('local duel: the CHAOS MODE box (ticks, remembered); the duel has Chaos Mode')


def chaos_rounds(k='A'):
    """Both players' first shots go without an effect; then each round's effect (announced: its name in the middle,
    the gong) lasts until both have shot, and the next round's replaces it."""
    since = mark()
    for i in range(2):
        chaos_shoot(k, 300 + 200 * i, 420)
        chaos_next_turn(k)
        s = chaos_state(k)
        if i == 0 and s['effect'] != '-1':
            raise StepFailed(f'an effect began after only the first shot: {s}')
    s = chaos_state(k)
    if s['round'] != '1' or s['effect'] == '-1':
        raise StepFailed(f'after both first shots, round 1 should have begun with an effect: {s}')
    first = int(s['effect'])
    wait_log(k, f'chaos: round 1: {CHAOS_EFFECTS[first]}', since, 3)
    time.sleep(0.4)
    frame(k, 'chaos_announced')
    for i in range(2):
        chaos_shoot(k, 350 + 100 * i, 430)
        chaos_next_turn(k)
        if i == 0 and chaos_state(k)['effect'] != str(first):
            raise StepFailed("the round's effect changed before both players had shot")
    s = chaos_state(k)
    if s['round'] != '2' or s['effect'] in ('-1', str(first)):
        raise StepFailed(f'round 2 should have brought a new effect: {s}')
    ok(f'rounds: two shots without an effect, then {CHAOS_EFFECTS[first]}, then {CHAOS_EFFECTS[int(s["effect"])]}')


def chaos_zen(k='A'):
    """Master Hu's Zen Ball (given to the player in turn, as his power would): the game chooses the shot, in a state
    of its own, before the ball is in play. That's the player's shot of the round, which the round's Double Shot
    splits in two, and after the other player's shot the next round begins. (Run where a round begins.)"""
    chaos_force(k, 8)                                        # Double Shot!
    s = chaos_state(k)
    if s['shots'] != '0':
        raise StepFailed(f'a round should just have begun: {s}')
    rnd, zen, twins = s['round'], int(s['zen']), int(chaos_fx(k)['twins'])
    send(k, 'chaos zen')
    wait(lambda: chaos_state(k)['zenleft'] == '1', 3, 'chaos zen gave the player no Zen Ball')
    balls = board(k)['balls']
    for attempt in range(3):                                 # (as chaos_shoot, but the Zen Ball takes its time)
        send(k, 'move 400 420')
        time.sleep(0.3)
        click(k, 400, 420)
        try:
            wait(lambda: board(k)['state'] != '1', 2.5, '')
            break
        except StepFailed:
            pass
    else:
        raise StepFailed('the Zen Ball shot did not go')
    wait(lambda: board(k)['balls'] != balls, 15, 'the Zen Ball never left the cannon')
    chaos_next_turn(k)
    s = chaos_state(k)
    if int(s['zen']) <= zen:
        raise StepFailed(f'the shot never went through the Zen Ball choosing it: {s}')
    if s['shots'] == '0' or s['round'] != rnd or s['effect'] != '8':
        raise StepFailed(f'the Zen Ball should be a shot of the round, with Double Shot still on: {s}')
    if int(chaos_fx(k)['twins']) <= twins:
        raise StepFailed("Double Shot did not split the Zen Ball's shot in two")
    chaos_shoot(k, 400, 420)
    chaos_next_turn(k)
    s = chaos_state(k)
    if s['round'] != str(int(rnd) + 1) or s['effect'] == '8':
        raise StepFailed(f"after the other player's shot the next round should have begun: {s}")
    ok(f'a Zen Ball is a shot of the round (Double Shot split it), then round {s["round"]}: '
       f'{CHAOS_EFFECTS[int(s["effect"])]}')


def chaos_effects(k='A'):
    """Every effect in turn (kept through the rounds meanwhile), each checked by what the game does with it."""
    from PIL import ImageChops
    send(k, 'chaos hold')

    def grew(name, before, by=1):
        return int(chaos_fx(k)[name]) >= int(before[name]) + by

    def check(e, cond, what, timeout=10):
        wait(cond, timeout, f'{CHAOS_EFFECTS[e]} {what}')

    def spooky(e, x, y, flipped):
        """A shot that is a Spooky Ball (Renfield's power; `chaos spooky` makes the next shot one): out of the bottom
        it comes back at the top on its way down, upside down out of the top it comes back at the bottom on its way
        up. (A ball the bucket catches never falls out: another shot.)"""
        for attempt in range(3):
            chaos_next_turn(k)
            f = chaos_fx(k)
            send(k, 'chaos spooky')
            chaos_shoot(k, x, y)
            try:
                wait(lambda: grew('spooky', f), 15, '')
            except StepFailed:
                chaos_next_turn(k)
                continue
            g = chaos_fx(k)
            sy = float(g['spookyat'].split(',')[1])
            vy = float(g['spookyv'].split(',')[1])
            want = 40 + (550 if flipped else 10)
            used = int(g['spookywas']) - int(g['spookyleft'])  # (one: Renfield's power may have added more)
            if abs(sy - want) > 1 or (vy >= 0 if flipped else vy <= 0) or used != 1:
                raise StepFailed(f'{CHAOS_EFFECTS[e]}: the Spooky Ball should have come back at y {want}, on its way '
                                 f'{"up" if flipped else "down"}, using one return: {g}')
            time.sleep(0.15)
            frame(k, 'chaos_spooky_' + ('upside_down' if flipped else 'normal'))
            chaos_next_turn(k)
            return
        raise StepFailed(f'{CHAOS_EFFECTS[e]}: three shots and the Spooky Ball never came back')

    chaos_force(k, 0)
    check(0, lambda: chaos_fx(k)['radius'] == '9.0', 'did not make the ball in the cannon 9 across (from 6)')
    frame(k, 'chaos_big_balls')
    chaos_force(k, 1)
    f = chaos_fx(k)
    if f['cap'] != '22':
        raise StepFailed(f"Pinball Madness should raise the speed limit to 22, not {f['cap']}")
    chaos_shoot(k, 360, 420)
    check(1, lambda: grew('kicks', f), 'kicked no ball off a peg', 15)
    chaos_next_turn(k)
    chaos_force(k, 2)
    check(2, lambda: chaos_fx(k)['parts'] == '11', 'did not find the bucket')
    f = chaos_fx(k)
    if float(f['hole'].split(',')[0]) > 330:
        raise StepFailed(f"Double Bucket: the bucket should keep to the left half, it is at {f['hole']}")
    time.sleep(0.5)
    frame(k, 'chaos_double_bucket')
    send(k, 'chaos dropin')
    chaos_shoot(k, 400, 300)
    check(2, lambda: grew('catches', f), 'did not catch a ball in the second bucket')
    chaos_next_turn(k)
    chaos_force(k, 3)
    f = chaos_fx(k)
    check(3, lambda: grew('jumps', f), 'did not make the bucket jump', 5)
    time.sleep(0.15)
    frame(k, 'chaos_teleporting_bucket')
    chaos_force(k, 4)
    check(4, lambda: chaos_fx(k)['purples'] == '2', 'did not make two more purple pegs')
    frame(k, 'chaos_purple_party')
    since = mark()
    chaos_force(k, 5)
    wait_log(k, 'changed color', since, 3)
    if any('chaos: shuffled' in line and ' 0 changed color' in line for line in log_lines(k)[since[k]:]):
        raise StepFailed('Peg Shuffle changed no peg')
    time.sleep(0.5)
    frame(k, 'chaos_peg_shuffle')
    chaos_force(k, 'none')
    time.sleep(0.3)
    plain = frame(k, 'chaos_plain').crop((90, 140, 710, 540))
    chaos_force(k, 6)
    time.sleep(3.2)                                          # after the announcement
    hidden = frame(k, 'chaos_invisible_pegs').crop((90, 140, 710, 540))
    changed = sum(1 for v in ImageChops.difference(plain, hidden).convert('L').getdata() if v > 40)
    if changed < 5000:
        raise StepFailed(f'Invisible Pegs: the pegs are still to be seen ({changed} pixels changed)')
    chaos_force(k, 7)
    send(k, 'move 400 300')                                  # (the guide shows with the mouse on the board)
    f = chaos_fx(k)
    check(7, lambda: grew('blind', f, 10), 'did not leave the aiming guide out', 3)
    spooky(7, 400, 420, False)                               # the game's own Spooky Ball, gravity as usual
    chaos_force(k, 8)
    f = chaos_fx(k)
    chaos_shoot(k, 400, 420)
    check(8, lambda: grew('twins', f) and chaos_fx(k)['inplay'] == '2', 'did not fire a second ball', 3)
    time.sleep(0.8)
    frame(k, 'chaos_double_shot')
    chaos_next_turn(k)
    chaos_force(k, 9)
    check(9, lambda: chaos_fx(k)['portals'] == '2', 'did not open two portals')
    time.sleep(3)
    frame(k, 'chaos_wormholes')
    chaos_force(k, 14)
    check(14, lambda: chaos_fx(k)['portals'] == '6', 'did not open six portals')
    time.sleep(3)
    frame(k, 'chaos_portal_storm')
    chaos_force(k, 10)
    f = chaos_fx(k)
    if f['walls'] != '0,0,1':
        raise StepFailed(f"Wrap Around should take the side walls away (left, right, ceiling): {f['walls']}")
    chaos_shoot(k, 80, 110)                                  # flat at the left wall
    check(10, lambda: grew('wraps', f), 'did not bring the ball round to the other side')
    time.sleep(0.2)
    frame(k, 'chaos_wrap_around')
    chaos_next_turn(k)
    since = mark()
    chaos_force(k, 11)
    send(k, 'chaos bombat 400 160')                          # a peg the shot can reach
    time.sleep(3)
    frame(k, 'chaos_bomb_peg')
    bx, by = (int(float(v)) for v in chaos_fx(k)['bombat'].split(','))
    chaos_shoot(k, bx, by)
    wait_log(k, 'the bomb peg went off', since, 10, 'the Bomb Peg did not go off when hit')
    time.sleep(0.3)
    frame(k, 'chaos_bomb_blast')
    chaos_next_turn(k)
    chaos_force(k, 'none')
    time.sleep(0.3)
    lit = playfield_brightness(frame(k, 'chaos_plain2'))
    chaos_force(k, 12)
    time.sleep(3.2)
    dark = playfield_brightness(frame(k, 'chaos_lights_out'))
    if dark > lit * 0.5:
        raise StepFailed(f'Lights Out: the board is not dark ({dark:.0f} against {lit:.0f} with the lights on)')
    chaos_force(k, 13)
    f = chaos_fx(k)
    if f['flipped'] != '1' or f['gun'] != '325,535' or not f['hole'].endswith(',30'):
        raise StepFailed(f'Upside Down: the cannon should be at the bottom and the bucket at the top: {f}')
    time.sleep(3)
    frame(k, 'chaos_upside_down')
    chaos_shoot(k, 400, 250)
    check(13, lambda: -1 < float(chaos_fx(k)['ball'].split(',')[1]) < 300, 'did not make the ball fall up', 6)
    frame(k, 'chaos_upside_down_shot')
    chaos_next_turn(k)
    spooky(13, 400, 250, True)
    chaos_force(k, 15)
    f = chaos_fx(k)
    for attempt in range(2):                                 # straight at an orange peg
        ox, oy = (int(float(v)) for v in chaos_fx(k)['orange'].split(','))
        chaos_shoot(k, ox, oy)
        try:
            check(15, lambda: grew('slowed', f), f'never slowed the game down near the orange peg at ({ox},{oy})', 12)
            break
        except StepFailed:
            chaos_next_turn(k)
            if attempt == 1:
                raise
    wait(lambda: chaos_fx(k)['slowmo'] == '1', 3, 'Bullet Time: no slow motion')
    frame(k, 'chaos_bullet_time')
    chaos_next_turn(k)
    chaos_force(k, 'none')
    send(k, 'chaos hold off')
    f = chaos_fx(k)
    if f['flipped'] != '0' or f['cap'] != '15':
        raise StepFailed(f'with the effects over, the game should be back to normal: {f}')
    ok('every effect did its part (local duel)')


def chaos_powers(k='A'):
    """Masters' powers at the bucket with the bucket's effects (`chaos power` gives the player in turn a power as a
    turn would, `chaos ballat` puts the ball in play somewhere): Claude's flippers upside down (at the top, mirrored,
    swinging down, batting a ball down) and back; Double Bucket's halves keeping half the track over the turns (half
    the flippers' narrower one too); the second bucket bouncing a ball off its rim and catching one off its middle;
    its lid knocked off by a fireball; Chain Lightning's arc to the nearer bucket, and upside down to the rod under
    the bucket; the pyramid's sides upside down; a catch in the bucket upside down."""
    send(k, 'chaos hold')

    def fx():
        return chaos_fx(k)

    def pair(key, f=None):
        return tuple(float(v) for v in (f or fx())[key].split(','))

    def place(x, y, vx, vy):
        send(k, f'chaos ballat {x:.0f} {y:.0f} {vx} {vy}')

    def path(n, every=0.04):  # where the ball in play goes (screen), until it's gone
        out = []
        for _ in range(n):
            time.sleep(every)
            b = pair('ball')
            if b == (-1, -1):
                break
            out.append(b)
        return out

    def track_kept(want, what):
        wait(lambda: fx()['track'] == str(want), 6, f'Double Bucket: the track should stay {want} wide {what}, '
             f"not {fx()['track']}")

    # Claude's flippers: at the bottom, then mirrored to the top upside down.
    chaos_next_turn(k)
    send(k, 'chaos power 2 3')
    f = fx()
    pivots = pair('flippers', f)
    if f['flipalive'] != '2' or not (520 < pivots[1] < 540 and 520 < pivots[3] < 540):
        raise StepFailed(f"Flippers: both claws should be at the bottom: {f['flippers']}, alive {f['flipalive']}")
    turns = f['flipturn']
    chaos_force(k, 13)
    time.sleep(3.2)
    f = fx()
    up = pair('flippers', f)
    want = (pivots[0], 666 - pivots[1], pivots[2], 666 - pivots[3])  # (mirrored about 586 / 2 + 40, as the bucket)
    if any(abs(a - b) > 1 for a, b in zip(up, want)) or f['flipturn'] == turns:
        raise StepFailed(f"Flippers upside down: the pivots should be at {want}, turning the other way: {f}")
    frame(k, 'powers_flippers_upside_down')
    rest = pair('flipreach', f)
    chaos_shoot(k, 400, 250)
    send(k, 'down 400 300')
    time.sleep(0.35)
    held = pair('flipreach')
    send(k, 'up 400 300')
    if held[1] < rest[1] + 25:
        raise StepFailed(f'Flippers upside down: flipping should swing them down (reach {rest} -> {held})')
    time.sleep(0.4)
    place(120, 156, 0, -0.5)                                 # under the left claw (resting at y ~150), flipped
    time.sleep(0.1)
    send(k, 'down 400 300')
    p = path(8)
    send(k, 'up 400 300')
    if not p or max(y for _, y in p) < 185:                  # (pressed up against it, it would stay over ~165)
        raise StepFailed(f'Flippers upside down: the claw should bat the ball down: {p}')
    ok('Flippers upside down: at the top, mirrored, swinging and batting the ball down')
    chaos_next_turn(k)
    chaos_force(k, 'none')
    time.sleep(0.5)
    back = pair('flippers')
    if any(abs(a - b) > 1 for a, b in zip(back, pivots)):
        raise StepFailed(f'Flippers: back at the bottom after Upside Down: {back}, not {pivots}')
    for player in (0, 1):                                    # (the one given them still has some)
        send(k, f'chaos power 2 0 {player}')

    # Double Bucket: half the track, kept so over the turns (the game sets a full one each turn).
    chaos_force(k, 2)
    track_kept(130, 'as it begins')
    send(k, 'chaos power 2 3')
    track_kept(65, 'with the flippers (half their 130)')
    send(k, 'chaos power 2 0')
    track_kept(130, 'with the flippers gone')
    frame(k, 'powers_double_bucket')
    f = fx()
    catches, bounces = int(f['catches']), int(f['twinbounces'])
    chaos_shoot(k, 400, 300)
    hx = pair('hole')[0]
    place(hx + 260 + 75 + 66, 560, 0, 3)                     # on the second bucket's right rim
    p = path(10)
    if int(fx()['twinbounces']) <= bounces:
        raise StepFailed(f'Double Bucket: a ball on the second bucket\'s rim should bounce off it: {p}')
    hx = pair('hole')[0]
    place(hx + 260 + 75 + 40, 584, 0, 1)                     # in, off its middle
    wait(lambda: int(fx()['catches']) > catches, 5, 'Double Bucket: the second bucket did not catch a ball off its '
         'middle')
    chaos_next_turn(k)
    track_kept(130, 'after the turn passed')
    send(k, 'chaos power 12 1')                              # a fireball, and the bucket's lid on
    f = fx()
    if f['lid'] != '1' or f['fireball'] != '1':
        raise StepFailed(f"Fireball: the lid should be on and the ball a fireball: lid {f['lid']}, {f['fireball']}")
    frame(k, 'powers_double_bucket_lid')
    knocks = int(f['lidknocks'])
    chaos_shoot(k, 400, 300)
    hx = pair('hole')[0]
    place(hx + 260 + 75, 545, 0, 2)                          # on the second bucket's lid
    wait(lambda: int(fx()['lidknocks']) > knocks and fx()['lid'] == '0', 5,
         'Double Bucket: a fireball on the second bucket\'s lid should knock it off')
    ok('Double Bucket: half the track over the turns, the second bucket\'s rim, catch and lid')

    # Chain Lightning: the arc to the nearer bucket.
    for attempt in range(3):
        chaos_next_turn(k)
        f = fx()
        zaps = int(f['zaps'])
        send(k, 'chaos power 14 1')
        wait(lambda: fx()['charged'] == '1', 3, 'Chain Lightning: the ball was not charged')
        if attempt == 0:
            time.sleep(1.5)                                  # (the rods rise)
            frame(k, 'powers_lightning_double_bucket')
        chaos_shoot(k, 690, 330)                             # (the right: the second bucket's side)
        wait(lambda: int(fx()['zaps']) > zaps, 10, 'Chain Lightning: no arc')
        f = fx()
        peg, rod = pair('zappeg', f)[0], float(f['zaprod'])
        nearer = abs(peg - rod - 260) < abs(peg - rod)
        if (f['zaptwin'] == '1') != nearer:
            raise StepFailed(f"Chain Lightning: the arc from a peg at x {peg:.0f} should go to the "
                             f"{'second' if nearer else 'first'} bucket (rod at {rod:.0f}): {f['zaptwin']}")
        if nearer:
            break
    else:
        raise StepFailed('Chain Lightning: three arcs and none from nearer the second bucket')

    # Upside down: the arc to the rod under the bucket; the pyramid's sides; a catch.
    chaos_next_turn(k)
    chaos_force(k, 13)
    time.sleep(3.2)
    zaps = int(fx()['zaps'])
    send(k, 'chaos power 14 1')
    wait(lambda: fx()['charged'] == '1', 3, 'Chain Lightning upside down: the ball was not charged')
    time.sleep(1.5)
    frame(k, 'powers_lightning_upside_down')
    chaos_shoot(k, *pair('orange'))                          # (at a peg: straight up from the cannon is a gap)
    wait(lambda: int(fx()['zaps']) > zaps, 10, 'Chain Lightning upside down: no arc')
    at = pair('zapat')
    if not 100 < at[1] < 125:                                # (the rod 44 under the bucket's middle at 70)
        raise StepFailed(f'Chain Lightning upside down: the arc should end at the rod under the bucket, not {at}')
    ok('Chain Lightning: the arc to the nearer bucket; upside down to the rod under the bucket')
    chaos_next_turn(k)
    send(k, 'chaos power 4 3')
    time.sleep(0.5)
    frame(k, 'powers_pyramid_upside_down')
    catches = int(fx()['upcatches'])
    chaos_shoot(k, 690, 330)
    wait(lambda: pair('hole')[0] > 220, 8, 'the bucket never came far enough right')
    hx = pair('hole')[0]
    place(hx - 130 + 75, 125, 0, -2)                         # under the pyramid's left side, rising
    p = path(8)
    top = min((y for _, y in p), default=0)                  # (its underside slopes, 84..93 there; through it the
    if top < 80 or not any(y > top + 8 for _, y in p[p.index(min(p, key=lambda q: q[1])):]):  # ball would go on up)
        raise StepFailed(f'Pyramid upside down: a rising ball should bounce off the underside of its side: {p}')
    hx = pair('hole')[0]
    place(hx + 75, 76, 0, -1)                                # in the bucket
    wait(lambda: int(fx()['upcatches']) > catches, 5, 'Upside Down: the bucket did not catch a ball')
    time.sleep(0.15)
    frame(k, 'powers_catch_upside_down')
    for player in (0, 1):
        send(k, f'chaos power 4 0 {player}')
    chaos_next_turn(k)
    chaos_force(k, 'none')
    send(k, 'chaos hold off')
    ok('the pyramid\'s sides and a catch upside down')


def chaos_leave(k='A'):
    leave_board(k)
    time.sleep(2)
    for line in log_lines(k):
        if 'EXCEPTION' in line:
            raise StepFailed(f'copy {k} logged an exception: {line}')
    ok('left the duel; no exceptions in the log')


def chaoslocal():
    k = 'A'
    chaos_local_start(k)
    chaos_rounds(k)
    chaos_zen(k)
    leave_board(k)                                           # a new duel: all of its balls for the effects
    time.sleep(3)
    chaos_local_start(k, check_box=False)
    chaos_effects(k)
    chaos_leave(k)
    time.sleep(3)
    chaos_local_start(k, check_box=False)                    # masters' powers at the bucket
    chaos_powers(k)
    chaos_leave(k)
    time.sleep(3)
    chaos_local_start(k, check_box=False)                    # and again for their Instant Replays
    path = chaos_replays(k)
    chaos_leave(k)
    try:
        time.sleep(2)
        replays_viewer(k, 'the saved Upside Down shot', path, saved_upside_down(k))
    finally:
        if path and os.path.exists(path):
            os.remove(path)
    ok('Chaos Mode (local) passed')


def saved_upside_down(k):
    def check():
        f = chaos_fx(k)
        if f['flipped'] != '1' or not int(f['active'], 16) >> 13 & 1:
            raise StepFailed(f'the saved Upside Down shot does not play upside down: {f}')
    return check


def shooter():
    """The copy whose player is aiming online, or None."""
    for k in 'AB':
        s = state(k)
        if s.get('control') == '1' and s.get('state') == '1' and s.get('seldlg') == '0':
            return k
    return None


def chaos_online_box():
    """The joiner's box shows the host's, and takes no clicks."""
    for k in 'AB':
        if not chaos_box(k):
            raise StepFailed(f'copy {k} has no CHAOS MODE box on the online selection')
    for want in (not chaos_box('A')[2], True):
        if chaos_box('A')[2] != want:
            x, y, _ = chaos_box('A')
            click('A', x, y)
        wait(lambda: chaos_box('A')[2] == want and chaos_box('B')[2] == want, 5,
             f"the joiner's box did not follow the host's ({want})")
    x, y, _ = chaos_box('B')
    click('B', x, y)
    time.sleep(0.8)
    if not chaos_box('B')[2] or chaos_state('B')['box'].split(',')[2] != '0':
        raise StepFailed("the joiner's box should stay as the host has it and take no clicks")
    frame('B', 'chaos_online_selection')
    ok("online selection: the joiner's CHAOS MODE box shows the host's choice")


def chaos_online_round(effect, done):
    """Once a round's effect is in play on both: the same on both, and the same picture; the bomb is hit."""
    a, b = chaos_state('A'), chaos_state('B')
    if a['effect'] != b['effect'] or a['round'] != b['round']:
        raise StepFailed(f'the two games have different effects: A {a}, B {b}')
    if effect == 11 and 'bomb' not in done:                  # set the bomb off: moved on both at the same tick
        for attempt in range(2):                             # (the round's two shots; one may hit other pegs first)
            k = wait(shooter, 60, 'no one gets to shoot')
            if chaos_state(k)['effect'] != '11':
                break
            at = max(int(send(c, 'tick')) for c in 'AB') + 50
            for c in 'AB':
                send(c, f'chaos bombat 400 160 {at}')
            wait(lambda: all(int(send(c, 'tick')) > at for c in 'AB'), 15, f'the games did not get past tick {at}')
            bx, by = (int(float(v)) for v in chaos_fx(k)['bombat'].split(','))
            since = mark()
            send(k, f'move {bx} {by}')
            time.sleep(0.4)
            click(k, bx, by)
            try:
                for c in 'AB':
                    wait_log(c, 'the bomb peg went off', since, 15, '')
                done.add('bomb')
                break
            except StepFailed:
                print(f'  copy {k} missed the bomb at ({bx},{by})', flush=True)
                wait(lambda: shooter() not in (None, k), 60, 'the turn never passed')
        if 'bomb' not in done:
            print('  (both shots missed the bomb: its blast unchecked this time)', flush=True)
    synced(f'chaos_online_{effect}')


def chaos_online():
    """Both copies: an online Chaos Mode duel deals the same effects on both computers, and their pictures stay the
    same through each. Duels one after the other (Replay Level) cover all 16 effects, queued on both in order."""
    order = [11, 13, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15]
    chaos_online_box()
    for k in 'AB':
        send(k, 'chaos next ' + ' '.join(map(str, order)))
    seen, done = [], set()
    for n in range(5):
        duel()
        shot = 0
        while True:
            for k in 'AB':
                if state(k).get('desync') == '1':
                    raise StepFailed(f'the games went out of sync (effects so far: {seen})')
            e = int(chaos_state('A')['effect'])
            if e >= 0 and e not in seen and chaos_state('B')['effect'] == str(e):
                seen.append(e)
                print(f'  effect {len(seen)}: {CHAOS_EFFECTS[e]}', flush=True)
                chaos_online_round(e, done)
            if autoplay(1, seed=200 + 13 * n + shot) == 'results':
                break
            shot += 1
        results_settled()
        for k in 'AB':
            send(k, 'move 760 300')                          # (a button under a pointer lights up on that screen only)
        time.sleep(0.3)
        synced(f'chaos_online_results{n}')
        if len(seen) == len(order):
            break
        since = mark()
        click('A', 513, 413)                                 # host: Replay Level
        wait_log('A', 'results button 1001 pressed (host decides)', since, 5)
        wait(lambda: all(state(k).get('seldlg') == '1' for k in 'AB'), 20, 'Replay Level brought no selection back')
        time.sleep(2)
        for k in 'AB':  # (one that couldn't come up, e.g. Purple Party on a nearly cleared board, is queued again)
            send(k, 'chaos next ' + ' '.join(str(e) for e in order if e not in seen))
        for k in 'AB':
            if not chaos_box(k) or not chaos_box(k)[2]:
                raise StepFailed(f'copy {k}: CHAOS MODE should still be ticked on the replayed level')
    missing = [CHAOS_EFFECTS[e] for e in order if e not in seen]
    if missing:
        raise StepFailed(f'these effects never came up online: {missing}')
    ok(f'online: all {len(seen)} effects the same on both computers, pictures identical')
    since = mark()
    at = button_center('B', 'Main Menu')                     # the joiner leaves from the results
    if not at:
        raise StepFailed("the results have no Main Menu button for the joiner")
    click('B', *at)
    wait_log('A', 'left the game', since, 10, 'copy A was not told that B left')
    ok('Chaos Mode (online) passed')


# ---- Instant Replay with the effects (local), saved replays ----

def replay_state(k):
    """The mod's replay bookkeeping on copy k: the shots it kept effects for, replays played and finished, the last
    replay's trace of where its balls were after each step against the shot's own (match), files read (with our
    states), saves (the last one's path), and online the SAVE button's center."""
    r = send(k, 'replay')
    out = dict(kv.split('=', 1) for kv in r.split() if '=' in kv)
    out['lastsaved'] = r.split('lastsaved=', 1)[1] if 'lastsaved=' in r else ''
    return out


def replay_button(k):
    """The board's Instant Replay button: (center x, center y, enabled), or None."""
    r = send(k, 'replay button')
    if r == 'none':
        return None
    x, y, w, h, shown, enabled = r.split()
    return int(x) + int(w) // 2, int(y) + int(h) // 2, shown == 'shown=1' and enabled == 'enabled=1'


REPLAY_SAVE_AT = (761, 492)  # the replay panel's Save, between Instant Replay and Done


def panel_save(k, what, keep=False):
    """The replay panel's Save: the game's own file dialog, its own name for the file; the file then ends with the
    mod's states too. (Deleted again, unless kept: then its path is returned.)"""
    before = replay_state(k)

    def dialog_save():  # (the panel's own button says Save too)
        return next((c for c in button_centers(k, 'Save') if c[0] < 700), None)
    click(k, *REPLAY_SAVE_AT)
    wait(dialog_save, 8, f"{what}: the panel's Save opened no file dialog")
    time.sleep(1.5)                                          # (it slides in)
    frame(k, 'replay_save_dialog')
    click(k, *dialog_save())
    time.sleep(1.5)
    if button_center(k, 'Save Single'):                      # (all the shots or this one)
        click(k, *button_center(k, 'Save Single'))
    wait(lambda: int(replay_state(k)['saves']) > int(before['saves']), 10, f'{what}: the saved file got no states')
    path = replay_state(k)['lastsaved']
    has = replay_file_has_states(path)
    if not keep:
        os.remove(path)
    if not has:
        raise StepFailed(f'{what}: {path} does not end with the states')
    wait(lambda: not dialog_save(), 10, f'{what}: the file dialog did not close')
    ok(f"{what}: the replay panel's Save wrote {os.path.basename(path)} with the effects kept")
    return path


def instant_replay(k, what, name=None, during=None, save=False, keep=False):
    """The last shot again: Instant Replay plays it with the effects it had (`during` checks them meanwhile), its
    balls where the shot's were after every step; Done brings the game back as it was. With `save`, the panel's
    Save too (the file's path returned if kept)."""
    wait(lambda: board(k)['state'] == '1' and board(k)['replay'] == '0', 60, f'{what}: no one is aiming')
    b = wait(lambda: (lambda b: b if b and b[2] else None)(replay_button(k)), 10, f'{what}: no Instant Replay to press')
    before = int(replay_state(k)['finished'])
    click(k, b[0], b[1])
    wait(lambda: board(k)['replay'] == '1', 5, f'{what}: Instant Replay did not start')
    time.sleep(0.8)
    if during:
        during()
    if name:
        frame(k, f'replay_{name}')
    wait(lambda: int(replay_state(k)['finished']) > before, 60, f'{what}: the replay never finished')
    r = replay_state(k)
    if r['match'] != '1':
        raise StepFailed(f"{what}: the replay's balls did not go where the shot's did: {r}")
    path = panel_save(k, what, keep) if save else None
    click(k, *map(int, REPLAY_DONE_AT.split(',')))           # Done
    wait(lambda: board(k)['replay'] == '0', 10, f'{what}: Done did not end the replay')
    wait(lambda: board(k)['state'] == '1', 10, f'{what}: after the replay the turn did not come back')
    r['path'] = path
    return r


def local_duel_again(k='A'):
    """The local duel is over (no balls left): Replay Level and Play Now, for more shots."""
    if not button_center(k, 'Replay Level'):
        return
    click(k, *button_center(k, 'Replay Level'))
    wait(lambda: dialog_open(k, DLG_SELECTION), 15, 'Replay Level brought no master selection')
    time.sleep(1.5)
    click(k, 400, 547)                                       # Play Now
    wait(lambda: board(k)['state'] == '1', 30, 'the replayed level never got to the first shot')
    print('  (the duel was over: the level again)', flush=True)


def chaos_replays(k='A', effects=range(16)):
    """Each effect in turn: a shot, then its Instant Replay, with the effect in place meanwhile and the balls going
    just where they went; afterwards the game is back with the effect still on. Upside Down's (or the last's) is saved
    with the replay panel's Save: its path is returned, to play from Replays."""
    effects = list(effects)
    keep = len(effects) - 1 - effects[::-1].index(13 if 13 in effects else effects[-1])  # (one file saved)
    kept = None
    send(k, 'chaos hold')
    for i, e in enumerate(effects):
        for attempt in range(3):                             # (the duel's last shot brings the results instead)
            wait(lambda: board(k)['state'] == '1' or button_center(k, 'Replay Level'), 90, 'no one gets to shoot')
            local_duel_again(k)
            chaos_force(k, e)
            if e == 11:                                      # straight at the bomb
                send(k, 'chaos bombat 400 160')
                time.sleep(0.4)
                x, y = fx_point(chaos_fx(k), 'bombat')
            elif e == 10:                                    # flat at the left wall
                x, y = 80, 110
            elif e == 2:                                     # (not `chaos dropin`: the test's jump isn't a shot's)
                x, y = 640, 520
            else:
                x, y = 400, 420
            chaos_shoot(k, x, y)
            wait(lambda: board(k)['state'] == '1' or button_center(k, 'Replay Level'), 90, 'the next turn never came')
            if board(k)['state'] == '1':
                break
        live = chaos_fx(k)

        def during():
            a = int(chaos_fx(k)['active'], 16)
            if not a >> e & 1:
                raise StepFailed(f'{CHAOS_EFFECTS[e]}: not in place during its replay (active {a:X})')
        r = instant_replay(k, CHAOS_EFFECTS[e], f'chaos_{e}', during, save=i == keep, keep=True)
        kept = r['path'] or kept
        after = chaos_fx(k)
        if after['active'] != live['active'] or chaos_state(k)['effect'] != str(e):
            raise StepFailed(f"{CHAOS_EFFECTS[e]}: not on as before after the replay: {after['active']} "
                             f"(was {live['active']})")
        if e == 13 and (after['flipped'] != '1' or after['gun'] != '325,535'):
            raise StepFailed(f'Upside Down: the board is not upside down after the replay: {after}')
        print(f'  {CHAOS_EFFECTS[e]}: replayed ({r["steps"]} steps, the same)', flush=True)
    chaos_force(k, 'none')
    send(k, 'chaos hold off')
    ok('Instant Replay of a shot under each effect: the balls went the same way, the effect back on after')
    return kept


def replay_file_has_states(path):
    with open(path, 'rb') as f:
        data = f.read()
    return data[-4:] == b'PNRS'


def replays_viewer(k, what, path, during=None):
    """Main menu > Replays > that file's row > Play: the saved replay plays with the effects it had (`during` checks
    them meanwhile), its balls where the shot's were; Done, then back to the main menu."""
    before = replay_state(k)
    names = sorted((f[:-5] for f in os.listdir(os.path.dirname(path)) if f.lower().endswith('.pegn')),
                   key=str.lower)                            # (the list's order)
    row = names.index(os.path.basename(path)[:-5])
    click(k, *button_center(k, 'Replays'))
    time.sleep(2)
    if button_center(k, 'Ok') and not button_center(k, 'Play'):  # a note about saving replays comes first
        click(k, *button_center(k, 'Ok'))
        time.sleep(2)
    at = wait(lambda: button_center(k, 'Play'), 10, 'Replays did not show the list of saved replays')
    time.sleep(1)
    buttons = {}                                             # the scroll bar's up (0) and down (1) buttons
    for line in send(k, 'widgets').splitlines():
        w = line.split()
        if w and w[0] == 'ScrollbuttonWidget' and 'v' in w[4]:
            x, y = map(int, w[1].split(','))
            buttons[w[5]] = (x + 8, y + 8)
    shown = row                                              # its row as shown: the list from its top (it opens at
    if 'id=0' in buttons:                                    # the last one chosen), scrolled down to it if need be
        for _ in range(len(names)):
            click(k, *buttons['id=0'])
            time.sleep(0.05)
        for _ in range(max(0, row - 6)):
            click(k, *buttons['id=1'])
            time.sleep(0.05)
        shown = min(row, 6)
    click(k, 300, 135 + 16 * shown)                          # (it keeps the last one chosen otherwise)
    time.sleep(0.5)
    frame(k, 'replays_list')
    click(k, *at)
    wait(lambda: int(replay_state(k)['filesread']) > int(before['filesread']), 10, f'{what}: no replay file was read')
    r = replay_state(k)
    if int(r['fileswithstates']) <= int(before['fileswithstates']):
        raise StepFailed(f'{what}: the replay file played has no effects kept (not the test\'s?): {r}')
    time.sleep(1)
    if during:
        during()
    frame(k, 'replays_viewer')
    wait(lambda: int(replay_state(k)['finished']) > int(before['finished']), 60, f'{what}: the saved replay never finished')
    r = replay_state(k)
    if r['match'] != '1':
        raise StepFailed(f"{what}: the saved replay's balls did not go where the shot's did: {r}")
    click(k, *map(int, REPLAY_DONE_AT.split(',')))           # Done: back to the list
    wait(lambda: button_center(k, 'Play'), 10, 'Done did not go back to the list of replays')
    time.sleep(1.5)
    for label in ('Main Menu', 'Cancel'):
        if button_center(k, label):
            click(k, *button_center(k, label))
            break
    wait(lambda: not button_center(k, 'Play') and button_center(k, 'Options'), 10, 'the replays list did not close')
    ok(f'{what}: the saved replay played from Replays with its effects, the balls where they went')


# ---- Red Peg Mode ----

RED_EFFECTS = {16: 'Mirage!', 17: 'Silhouette!', 7: 'Blind Shot!', 12: 'Lights Out!'}
RED_BOX_AREA = (330, 360, 470, 450)  # the box's center: the selection's middle column, above CHAOS MODE


def red_box(k):
    """The RED PEG MODE box on the selection: (center x, center y, ticked), or None."""
    r = send(k, 'chaos redbox')
    if r == 'none':
        return None
    x, y, w, h, checked = r.split()
    return int(x) + int(w) // 2, int(y) + int(h) // 2, checked == 'checked=1'


def set_box(k, box_of, want, key, name):
    """Tick or untick a box on the selection (by clicking it); netplay.ini remembers it."""
    if box_of(k)[2] != want:
        x, y, _ = box_of(k)
        click(k, x, y)
    wait(lambda: box_of(k)[2] == want, 3, f'clicking {name} did not make it {want}')
    wait(lambda: ini_value('duel', key) == ('1' if want else '0'), 3, f'netplay.ini did not remember {name} {want}')


def red_pixels(im, x, y, r=16):
    """Crimson pixels (the red peg's: an orange peg's have more green) round a point of the frame."""
    return sum(1 for p in im.crop((x - r, y - r, x + r, y + r)).getdata() if p[0] > 110 and p[1] < 40 and p[2] < 60)


def red_drawn(k, name, r=16):
    """A frame now, and the crimson pixels round the red peg in it (between where it was just before and just after:
    it may be on a path). Returns x, y, the count, the frame."""
    a = fx_point(chaos_fx(k), 'redat')
    im = frame(k, name)
    b = fx_point(chaos_fx(k), 'redat')
    x, y = (a[0] + b[0]) // 2, (a[1] + b[1]) // 2
    moved = max(abs(a[0] - b[0]), abs(a[1] - b[1]))
    return x, y, red_pixels(im, x, y, r + moved // 2 + 2), im


def red_kind(f):
    return ('a brick' if f['redbrick'] == '1' else 'a round peg') + (', moving' if f['redmoving'] == '1' else '')


def orange_pixels(im):
    """Orange-peg pixels on the playfield."""
    return sum(1 for p in im.crop((90, 120, 710, 560)).getdata() if p[0] > 170 and 30 < p[1] < 140 and p[2] < 70)


def fx_point(f, key):
    return tuple(int(float(v)) for v in f[key].split(','))


EIFFEL_POWER = (6, 660, 170)  # (stage, x, y): blue bricks and blue pegs going round
THE_GUARDIANS = (13, 400, 170)  # Spring Fever: Day 1, flowers with rings of blue bricks that turn round them


def shown_page(k):
    """The stage page the local duel's level screen shows: the player's profile keeps the last one turned to (+0x38,
    also between sessions), and the level screen opens on it."""
    return int(send(k, 'peek profile 38')) + 1


def level_page(k, page):
    """The local duel's level screen turned to a stage's page."""
    now = shown_page(k)
    for _ in range(abs(page - now)):
        click(k, 455 if page > now else 345, 563)            # next / previous stage
        time.sleep(1.2)
    if shown_page(k) != page:
        raise StepFailed(f'the level screen shows stage {shown_page(k)}, not {page}')


def level_screen_home(k='A'):
    """From the main menu: the local duel's level screen turned back to Stage One, then the main menu again. The
    player's profile keeps the page between sessions, so a test that ends on another stage would leave it there for
    the player."""
    click(k, 680, 372)                                       # Duel
    wait(lambda: dialog_open(k, DLG_MODE), 5, 'Duel did not open the Local/Online dialog')
    time.sleep(0.8)
    click(k, 215, 286)                                       # Local Play
    time.sleep(1.5)
    if dialog_open(k, DLG_CONTINUE_GAME):
        time.sleep(0.8)
        click(k, 440, 297)                                   # New Game
        wait(lambda: not dialog_open(k, DLG_CONTINUE_GAME), 5, 'New Game did not close "Continue Game?"')
    time.sleep(2.5)
    level_page(k, 1)
    click(k, *(button_center(k, 'Main Menu') or (32, 565)))  # the level screen's Main Menu
    wait(lambda: button_center(k, 'Options'), 10, 'the level screen did not go back to the main menu')
    time.sleep(1.0)


def red_local_start(k='A', chaos=False, check_box=True, level=(1, 140, 170)):
    """Copy A: Duel > Local Play > a level (the first). RED PEG MODE sits above CHAOS MODE, ticks and unticks and
    netplay.ini remembers it; Play Now with it ticked (and CHAOS MODE as asked) starts a duel with one red peg: a blue
    peg, drawn red."""
    click(k, 680, 372)                                       # Duel
    wait(lambda: dialog_open(k, DLG_MODE), 5, 'Duel did not open the Local/Online dialog')
    time.sleep(0.8)
    click(k, 215, 286)                                       # Local Play
    time.sleep(1.5)
    if dialog_open(k, DLG_CONTINUE_GAME):                    # an earlier local duel was left and saved
        time.sleep(0.8)
        click(k, 440, 297)                                   # New Game
        wait(lambda: not dialog_open(k, DLG_CONTINUE_GAME), 5, 'New Game did not close "Continue Game?"')
    time.sleep(2.5)
    level_page(k, level[0])
    click(k, level[1], level[2])
    wait(lambda: dialog_open(k, DLG_SELECTION), 10, 'the local duel never reached the master selection')
    time.sleep(1.5)                                          # slides in
    box, cbox = red_box(k), chaos_box(k)
    if not box or not cbox:
        raise StepFailed(f'the selection should have both boxes: RED PEG MODE {box}, CHAOS MODE {cbox}')
    x0, y0, x1, y1 = RED_BOX_AREA
    if not (x0 < box[0] < x1 and y0 < box[1] < y1 and box[1] < cbox[1] - 20):
        raise StepFailed(f'the RED PEG MODE box is at {box[:2]}, not above CHAOS MODE ({cbox[:2]}) in the middle')
    if check_box:
        frame(k, 'red_selection')
        for want in (not box[2], box[2]):                    # flip it, back
            set_box(k, red_box, want, 'red', 'RED PEG MODE')
    set_box(k, red_box, True, 'red', 'RED PEG MODE')
    set_box(k, chaos_box, chaos, 'chaos', 'CHAOS MODE')
    if check_box:
        frame(k, 'red_selection_ticked')
    click(k, 400, 547)                                       # Play Now
    wait(lambda: not dialog_open(k, DLG_SELECTION), 5, 'Play Now did not start the local duel')
    wait(lambda: board(k)['state'] == '1', 30, 'the duel never got to the first shot')
    s = chaos_state(k)
    if s['red'] != '1' or s['on'] != ('1' if chaos else '0') or s['running'] != '1' or s['redhit'] != '0':
        raise StepFailed(f'the duel should have Red Peg Mode{" and Chaos Mode" if chaos else ""}, running: {s}')
    f = chaos_fx(k)
    if int(f['redpeg']) < 0 or f['redtype'] != '1':
        raise StepFailed(f'the duel should have a red peg, a blue one underneath: {f}')
    time.sleep(1)                                            # (its glow closes in on it as the duel begins)
    rx, ry, n, _ = red_drawn(k, 'red_peg_chosen', 20)
    if n < (60 if f['redbrick'] == '0' else 30):
        raise StepFailed(f'the red peg ({red_kind(f)}) at ({rx},{ry}) is not drawn red ({n} red pixels)')
    ok(f'local duel: the RED PEG MODE box (above CHAOS MODE, ticks, remembered); a red peg at ({rx},{ry}), '
       f'{red_kind(chaos_fx(k))}')


def red_kinds(k='A'):
    """The red peg can be any blue peg: moved onto a brick, a moving peg and a still round one (those the level has),
    it's drawn red there with its glow. The frames are kept (red_kind_*: just after the move, its glow closing in)."""
    seen = []
    for kind in ('brick', 'moving', 'round'):
        since = mark()
        send(k, f'chaos redat 400 300 {kind}')
        wait_log(k, 'the red peg is moved', since, 5)
        if any('none of that kind' in line for line in log_lines(k)[since[k]:]):
            print(f'  (no blue {kind} peg on this level)', flush=True)
            continue
        f = chaos_fx(k)
        brick, moving = f['redbrick'] == '1', f['redmoving'] == '1'
        if brick != (kind == 'brick') or (kind != 'brick' and moving != (kind == 'moving')):
            raise StepFailed(f'the red peg should be {kind}: {red_kind(f)}')
        time.sleep(0.25)
        frame(k, f'red_kind_{kind}_intro')
        time.sleep(1.0)
        x, y, n, _ = red_drawn(k, f'red_kind_{kind}', 20)
        if n < 40:
            raise StepFailed(f'the red peg ({red_kind(f)}) at ({x},{y}) is not drawn red ({n} red pixels)')
        seen.append(f'{red_kind(f)} ({n} red pixels)')
    if not seen:
        raise StepFailed('the red peg could not be moved onto any blue peg')
    ok('the red peg is drawn red as ' + '; '.join(seen))


def red_hit(k, effect):
    """The red peg moved to the top middle (as if none had been hit yet) with its next effect queued; the player in
    turn shoots at it. Returns the frame before the shot and the other player, who gets the effect."""
    for attempt in range(4):
        chaos_next_turn(k)
        f = chaos_fx(k)
        if f['redtype'] not in ('1', '-1'):
            raise StepFailed(f'the red peg is not a blue peg underneath any more (made purple?): {f}')
        send(k, f'chaos rednext {effect}')
        since = mark()
        send(k, 'chaos redat 400 160')
        wait_log(k, 'the red peg is moved', since, 5)
        time.sleep(0.3)
        rx, ry = fx_point(chaos_fx(k), 'redat')
        plain = frame(k, 'red_before_hit')
        shooter = int(board(k)['player'])
        texts = int(chaos_fx(k)['redtexts'])
        chaos_shoot(k, rx, ry)
        try:
            wait_log(k, 'hit the red peg', since, 12, '')
        except StepFailed:
            print(f'  missed the red peg at ({rx},{ry}): again', flush=True)
            continue
        s = chaos_state(k)
        if s['redhit'] != '1' or s['victim'] != str(1 - shooter):
            raise StepFailed(f'player {shooter + 1} hit the red peg: the other player should get its effect: {s}')
        if int(chaos_fx(k)['redtexts']) != texts + 1:
            raise StepFailed(f'hitting the red peg should show "Sabotage!" once: {chaos_fx(k)["redtexts"]} texts '
                             f'(was {texts})')
        time.sleep(0.25)
        frame(k, 'red_hit')                                  # ("Sabotage!" over it)
        return plain, 1 - shooter
    raise StepFailed('four shots at the red peg and none hit it')


def red_effect_begins(k, effect, victim):
    """The next turn is the other player's, with the red peg's effect, announced."""
    chaos_next_turn(k)
    wait(lambda: chaos_state(k)['redeffect'] == str(effect), 5, f'{RED_EFFECTS[effect]} did not begin on the next turn')
    s = chaos_state(k)
    if s['player'] != str(victim) or s['redannounced'] == '-1':
        raise StepFailed(f'{RED_EFFECTS[effect]} should be on player {victim + 1}\'s turn, announced: {s}')
    time.sleep(0.5)
    frame(k, 'red_announced')


def red_effect_ends(k, effect):
    """After that one turn the effect is gone."""
    chaos_next_turn(k)
    s, f = chaos_state(k), chaos_fx(k)
    if s['redeffect'] != '-1' or int(f['active'], 16) >> effect & 1:
        raise StepFailed(f'{RED_EFFECTS[effect]} lasted more than the one turn: {s} / active {f["active"]}')


def guide_at_fake(k):
    """Aimed at a fake peg, the guide stops at a fake as at a real peg (going through would give it away): at that
    one, or another fake in the way. Each fake still there in turn until one has nothing real in the way. Returns
    where the fake it stopped at is."""
    fakes = [tuple(map(int, at.split(','))) for at in chaos_fx(k)['fakelist'].split(';') if at != '-']
    for x, y in fakes:
        n = int(chaos_fx(k)['guidefakes'])
        send(k, f'move {x} {y}')
        time.sleep(0.5)
        g = chaos_fx(k)
        if int(g['guidefakes']) <= n:
            continue
        step, gx, gy = (int(v) for v in g['guidefake'].split(','))
        near = min(fakes, key=lambda p: (p[0] - gx) ** 2 + (p[1] - gy) ** 2)
        if abs(near[0] - gx) > 45 or abs(near[1] - gy) > 45:
            raise StepFailed(f'the guide stopped at ({gx},{gy}), at no fake peg: {fakes}')
        frame(k, 'red_mirage_guide')
        ok(f'Mirage: aimed at the fake peg at ({x},{y}), the guide stops at the fake at {near} ({gx},{gy}, '
           f'step {step})')
        return near
    raise StepFailed(f'Mirage: aimed at each fake peg, the guide never stopped at one: {fakes}')


def red_effects(k='A'):
    """The red peg hit for real first (afterwards there is none), then moved back for each effect in turn."""
    # Mirage: as the turn passes the board goes black and the fake pegs appear in the dark (never in sight), then
    # it lights up again; they're gone after that turn; the ball goes through one and it glitches.
    plain, victim = red_hit(k, 16)
    black, hidden_seen, end = None, False, time.time() + 30
    while time.time() < end and black is None:
        f = chaos_fx(k)
        hidden_seen = hidden_seen or f['fakeshidden'] == '1'
        if f['black'] == '255':
            black = frame(k, 'red_mirage_black')
        elif chaos_state(k)['redeffect'] == '16' and f['fakeshidden'] == '0' and f['black'] == '0':
            raise StepFailed(f'Mirage began without the board going black first: {f}')
        time.sleep(0.04)
    if black is None:
        raise StepFailed('Mirage: the board never went black as the turn passed')
    dark = playfield_brightness(black)
    if dark > 6 or not hidden_seen:
        raise StepFailed(f'Mirage: the board should be black ({dark:.1f}) with the fakes hidden until then')
    wait(lambda: chaos_state(k)['redeffect'] == '16', 15, 'Mirage did not begin on the next turn')
    if chaos_fx(k)['black'] == '0':
        raise StepFailed('Mirage began after the board had lit up again: its fakes appeared in sight')
    wait(lambda: chaos_fx(k)['blackout'] == '0', 5, 'Mirage: the board did not light up again')
    ok(f'Mirage: the board went black as the turn passed (brightness {dark:.1f} against '
       f'{playfield_brightness(plain):.0f}), the fakes came in the dark, then it lit up')
    f = chaos_fx(k)
    if f['redat'] != '-1,-1' or chaos_state(k)['redhit'] != '1':
        raise StepFailed(f'the red peg should be gone after the shot that hit it: {f}')
    wait(lambda: chaos_state(k)['redeffect'] == '16', 5, 'Mirage did not begin on the next turn')
    s = chaos_state(k)
    if s['player'] != str(victim) or s['redannounced'] == '-1':
        raise StepFailed(f'Mirage should be on player {victim + 1}\'s turn, announced: {s}')
    time.sleep(0.5)
    frame(k, 'red_announced')
    f = chaos_fx(k)
    if int(f['fakes']) < 6:
        raise StepFailed(f'Mirage: too few fake pegs ({f["fakes"]})')
    in_places = f['fakesinplaces']
    if int(in_places) < 1:                                   # (the red peg's own place, at least)
        raise StepFailed(f'Mirage: no fake peg in a gone peg\'s place ({f["fakes"]} fakes)')
    ok(f'Mirage: {f["fakes"]} fake pegs, {in_places} in gone pegs\' places ({f["fakebricks"]} bricks)')
    time.sleep(2.8)                                          # after the announcement
    frame(k, 'red_mirage')
    fx, fy = guide_at_fake(k)
    f = chaos_fx(k)
    g0 = int(f['glitches'])
    chaos_shoot(k, fx, fy)
    end, glitched = time.time() + 8, False
    while time.time() < end and board(k)['state'] != '1':
        if int(chaos_fx(k)['glitches']) > g0:
            glitched = True
            time.sleep(0.12)
            frame(k, 'red_mirage_glitch')
            break
        time.sleep(0.05)
    if glitched:
        ok('Mirage: the ball went through a fake peg and it glitched')
    else:
        print(f'  (the shot at the fake peg at ({fx},{fy}) hit real pegs first)', flush=True)
    red_effect_ends(k, 16)
    if chaos_fx(k)['fakes'] != '0':
        raise StepFailed('Mirage: the fake pegs are still there after the turn')
    if chaos_state(k)['redhit'] != '1' or chaos_fx(k)['redat'] != '-1,-1':
        raise StepFailed('a red peg came back after it was hit')
    ok(f'Mirage: {f["fakes"]} fake pegs for player {victim + 1}\'s turn only; no red peg any more')
    if glitched:  # Mirage is over, but the Instant Replay of its shot has it again, the fake glitching again
        g_before = int(chaos_fx(k)['glitches'])

        def during():
            f = chaos_fx(k)
            if f['fakes'] == '0' or not int(f['active'], 16) >> 16 & 1:
                raise StepFailed(f'Mirage: no fake pegs in the replay of its shot: {f}')
            if f['fakesinplaces'] != in_places:
                raise StepFailed(f'Mirage: {f["fakesinplaces"]} fakes in gone pegs\' places in the replay, not '
                                 f'{in_places}')
        instant_replay(k, 'Mirage', 'red_mirage', during)
        f = chaos_fx(k)
        if int(f['glitches']) <= g_before:
            raise StepFailed("Mirage: in its shot's replay the ball didn't go through the fake again")
        if f['fakes'] != '0' or int(f['active'], 16) >> 16 & 1:
            raise StepFailed(f'Mirage is back on after the replay of its shot: {f}')
        ok("Mirage: its shot's Instant Replay had the fakes (one glitched again); they're gone again after")
    if not glitched:                                         # once more, until the ball goes through one
        for attempt in range(3):
            _, victim = red_hit(k, 16)
            red_effect_begins(k, 16, victim)
            f = chaos_fx(k)
            fx, fy = fx_point(f, 'fakeat')
            chaos_shoot(k, fx, fy)
            try:
                wait(lambda: int(chaos_fx(k)['glitches']) > int(f['glitches']), 8, '')
                time.sleep(0.1)
                frame(k, 'red_mirage_glitch')
                red_effect_ends(k, 16)
                break
            except StepFailed:
                red_effect_ends(k, 16)
        else:
            raise StepFailed('Mirage: four shots at fake pegs and the ball never went through one')
        ok('Mirage: the ball went through a fake peg and it glitched')

    # Silhouette: every peg gray (no orange to be seen), each hit one in its color.
    plain, victim = red_hit(k, 17)
    red_effect_begins(k, 17, victim)
    time.sleep(2.8)
    gray = frame(k, 'red_silhouette')
    before, during = orange_pixels(plain), orange_pixels(gray)
    if during > before * 0.3 or before - during < 150:
        raise StepFailed(f'Silhouette: the pegs kept their colors ({during} orange pixels against {before} before)')
    if int(chaos_fx(k)['gray']) == 0:
        raise StepFailed('Silhouette: no peg was drawn gray')
    chaos_shoot(k, 400, 330)
    wait(lambda: board(k)['state'] != '2', 15, 'Silhouette: the shot never ended')
    frame(k, 'red_silhouette_hit')
    red_effect_ends(k, 17)
    ok(f'Silhouette: gray pegs ({during} orange pixels against {before}) for one turn')

    # Blind Shot: no aiming guide for that turn, then it's back. First the Instant Replay of the shot that hit the
    # red peg: it's red in it and lights up again, without giving the effect again; after it Blind Shot is still on.
    plain, victim = red_hit(k, 7)
    red_effect_begins(k, 7, victim)
    drawn = int(chaos_fx(k)['reddrawn'])

    def red_in_replay():
        f = chaos_fx(k)
        if int(f['active'], 16) >> 7 & 1 or int(f['reddrawn']) <= drawn:
            raise StepFailed(f'the replay of the red peg hit should have the red peg and no Blind Shot: {f}')
    instant_replay(k, 'the shot that hit the red peg', 'red_hit', red_in_replay)
    s = chaos_state(k)
    if s['redeffect'] != '7' or s['victim'] != '-1' or s['redhit'] != '1' or not int(chaos_fx(k)['active'], 16) >> 7 & 1:
        raise StepFailed(f'after the replay Blind Shot should still be on (and nothing given again): {s}')
    ok('the Instant Replay of the shot that hit the red peg: red in it, nothing given again; Blind Shot still on')
    send(k, 'move 400 300')                                  # (the guide shows with the mouse on the board)
    f = chaos_fx(k)
    wait(lambda: int(chaos_fx(k)['blind']) >= int(f['blind']) + 10, 3, 'Blind Shot did not leave the guide out')
    frame(k, 'red_blind_shot')
    chaos_shoot(k, 400, 420)
    red_effect_ends(k, 7)
    send(k, 'move 400 300')
    b = chaos_fx(k)['blind']
    time.sleep(0.6)
    if chaos_fx(k)['blind'] != b:
        raise StepFailed('Blind Shot: the guide stayed hidden after the turn')
    ok('Blind Shot: no guide for one turn')

    # Lights Out: dark for that turn.
    plain, victim = red_hit(k, 12)
    red_effect_begins(k, 12, victim)
    time.sleep(2.8)
    lit, dark = playfield_brightness(plain), playfield_brightness(frame(k, 'red_lights_out'))
    if dark > lit * 0.5:
        raise StepFailed(f'Lights Out: the board is not dark ({dark:.0f} against {lit:.0f})')
    chaos_shoot(k, 400, 420)
    red_effect_ends(k, 12)
    ok('Lights Out: dark for one turn')


def red_with_chaos(k='A'):
    """Both modes. The round's second shooter hits the red peg: the first shooter's next turn brings the new round's
    effect and the red peg's together, both announced. The round's is Lights Out (queued), so the red peg's can't be
    (queued first): Silhouette. After that shot the red peg's is over and the round's stays for the other player."""
    for attempt in range(3):
        chaos_next_turn(k)
        if chaos_state(k)['shots'] == '0':                   # the round's first shot
            since = mark()
            send(k, 'chaos redat 620 500')                   # out of the way
            wait_log(k, 'the red peg is moved', since, 5)
            chaos_shoot(k, 150, 430)
            chaos_next_turn(k)
        s = chaos_state(k)
        first = 0 if s['shots'] == '1' else 1
        if s['shots'] not in ('1', '2') or s['player'] != str(1 - first):
            raise StepFailed(f'expected the round\'s second shot: {s}')
        send(k, 'chaos next lightsout')
        send(k, 'chaos rednext lightsout silhouette')
        since = mark()
        send(k, 'chaos redat 400 160')
        wait_log(k, 'the red peg is moved', since, 5)
        time.sleep(0.3)
        rx, ry = fx_point(chaos_fx(k), 'redat')
        chaos_shoot(k, rx, ry)
        try:
            wait_log(k, 'hit the red peg', since, 12, '')
        except StepFailed:
            print('  missed the red peg: again', flush=True)
            continue
        chaos_next_turn(k)
        wait(lambda: chaos_state(k)['redeffect'] != '-1', 5, "the red peg's effect did not begin")
        s = chaos_state(k)
        if s['player'] != str(first) or s['effect'] != '12' or s['redeffect'] != '17':
            raise StepFailed(f"Lights Out (the round's) and Silhouette (the red peg's: not Lights Out twice) should "
                             f"both be on player {first + 1}'s turn: {s}")
        if s['announced'] == '-1' or s['redannounced'] == '-1':
            raise StepFailed(f'both effects should be announced: {s}')
        time.sleep(0.6)
        frame(k, 'red_and_chaos_announced')
        active = int(chaos_fx(k)['active'], 16)
        if not (active >> 12 & 1 and active >> 17 & 1):
            raise StepFailed(f'both effects should be in play: active {active:X}')
        time.sleep(2.6)
        frame(k, 'red_and_chaos')
        chaos_shoot(k, 400, 420)
        chaos_next_turn(k)
        s = chaos_state(k)
        if s['effect'] != '12' or s['redeffect'] != '-1':
            raise StepFailed(f"the next turn: the round's Lights Out should stay, the red peg's Silhouette go: {s}")
        ok("both modes: the round's effect and the red peg's together (announced together, never the same one)")
        return
    raise StepFailed('three shots at the red peg and none hit it')


def red_bricks_moving(k='A'):
    """A level with blue bricks and moving blue pegs (Eiffel Power): the red peg drawn red as a brick, a moving peg
    and a round one. Then, with pegs gone, Mirage puts fakes in their places, drawn as those were (bricks as bricks),
    the moving ones going on along their paths; the guide stops at a fake; the ball goes through a brick fake and it
    glitches (the frames red_mirage_brick*)."""
    red_local_start(k, check_box=False, level=EIFFEL_POWER)
    f = chaos_fx(k)
    if int(f['bluebricks']) < 1 or int(f['bluemoving']) < 1:
        raise StepFailed(f'Eiffel Power should have blue bricks and moving blue pegs: {f["bluebricks"]} bricks, '
                         f'{f["bluemoving"]} moving')
    red_kinds(k)
    for x, y in ((250, 330), (550, 330), (400, 380)):        # pegs of every kind gone
        chaos_shoot(k, x, y)
        chaos_next_turn(k)
    for attempt in range(3):
        _, victim = red_hit(k, 16)
        red_effect_begins(k, 16, victim)
        f = chaos_fx(k)
        if int(f['fakebricks']) >= 1 and int(f['fakemoving']) >= 1:
            break
        print(f'  Mirage: {f["fakebricks"]} fakes in gone bricks\' places, {f["fakemoving"]} in moving ones\': '
              'again', flush=True)
        chaos_shoot(k, 400, 380)
        red_effect_ends(k, 16)
    else:
        raise StepFailed('three Mirages and none had fakes in both a gone brick\'s and a moving peg\'s place')
    time.sleep(2.8)                                          # after the announcement
    frame(k, 'red_mirage_bricks')
    before = chaos_fx(k)['fakelist']
    time.sleep(1.5)
    after = chaos_fx(k)['fakelist']
    if before == after:
        raise StepFailed(f'Mirage: the fakes in moving pegs\' places did not move: {after}')
    frame(k, 'red_mirage_bricks_later')
    ok(f'Mirage on Eiffel Power: {f["fakes"]} fakes, {f["fakesinplaces"]} in gone pegs\' places ({f["fakebricks"]} '
       f'bricks, {f["fakemoving"]} moving along their paths)')
    guide_at_fake(k)
    bx, by = fx_point(chaos_fx(k), 'fakebrickat')
    g0 = int(chaos_fx(k)['glitches'])
    chaos_shoot(k, bx, by)
    try:
        wait(lambda: int(chaos_fx(k)['glitches']) > g0, 8, '')
        for i, pause in enumerate((0.05, 0.2, 0.3)):
            time.sleep(pause)
            frame(k, f'red_mirage_brick_glitch{i + 1}')
        ok('Mirage: the ball went through a fake (aimed at one in a gone brick\'s place) and it glitched')
    except StepFailed:
        print(f'  (the shot at the brick fake at ({bx},{by}) hit real pegs first)', flush=True)
    red_effect_ends(k, 16)


def red_brick_turning(k='A'):
    """The red peg a brick turning round a flower (The Guardians): drawn red in every frame, its picture staying where
    it belongs in the brick. (Drawing its glow once lost that place, kept in xmm registers across the game's drawing,
    which changes them when it turns a picture: most frames drew the brick somewhere off the board.)"""
    red_local_start(k, check_box=False, level=THE_GUARDIANS)
    since = mark()
    send(k, 'chaos redat 404 244 brick')                     # the top of the middle flower's ring
    wait_log(k, 'the red peg is moved', since, 5)
    f = chaos_fx(k)
    if f['redbrick'] != '1' or f['redmoving'] != '1':
        raise StepFailed(f'the red peg should be a brick going round: {red_kind(f)}')
    time.sleep(1.0)                                          # (its glow closes in on it)
    pictures = {chaos_fx(k)['redpicture'] for _ in range(40)}
    if len(pictures) != 1:
        raise StepFailed(f"the red brick's picture moved about in it: {sorted(pictures)[:6]}")
    counts = [red_drawn(k, f'red_turning{i}', 20)[2] for i in range(10)]
    if min(counts) < 40:
        raise StepFailed(f'the turning red brick is missing from frames (red pixels in each: {counts})')
    ok(f'a red brick going round: its picture stays put ({pictures.pop()}) and 10 frames show it red '
       f'({min(counts)}-{max(counts)} red pixels)')


def redpeg_local():
    k = 'A'
    red_local_start(k)
    red_effects(k)
    chaos_leave(k)
    time.sleep(3)
    red_local_start(k, chaos=True, check_box=False)
    red_with_chaos(k)
    chaos_leave(k)
    time.sleep(3)
    red_bricks_moving(k)
    chaos_leave(k)
    time.sleep(3)
    red_brick_turning(k)
    chaos_leave(k)
    time.sleep(3)
    level_screen_home(k)                                     # (Eiffel Power is on Stage Six, The Guardians on 13)
    ok('Red Peg Mode (local) passed')


def online_save(saved):
    """Online, SAVE is where Instant Replay was: B saves its own last shot (a file named after it, with our states),
    noted in B's chat box only; pressed with A's ball in flight it waits for the shot to end. The games stay in sync,
    their pictures the same once the note has faded. (The paths go in `saved`, for deleting afterwards.)"""
    for attempt in range(8):                                 # B's shot the last one, A aiming
        k = wait(shooter, 60, 'no one gets to shoot')
        if k == 'A' and replay_state('B')['latestshooter'] == '1':
            break
        autoplay(1, seed=70 + attempt)
    else:
        raise StepFailed("B's shot never came last before A's turn")
    b = replay_state('B')
    if b['save'] == '-1,-1' or replay_state('A')['save'] == '-1,-1':
        raise StepFailed(f'no SAVE button online: A {replay_state("A")["save"]}, B {b["save"]}')
    frame('B', 'online_save_button')
    x, y = map(int, b['save'].split(','))
    click('B', x, y)
    wait(lambda: int(replay_state('B')['saves']) > int(b['saves']), 10, "B's SAVE wrote no replay file")
    path = replay_state('B')['lastsaved']
    saved.append(path)
    if not os.path.basename(path).startswith("Dad's shot") or not replay_file_has_states(path):
        raise StepFailed(f"B's saved shot should be \"Dad's shot\" with the effects kept: {path}")
    if not chat_state('B')['last'].startswith('Replay saved: Dad'):
        raise StepFailed(f"B's chat should say the replay was saved: {chat_state('B')}")
    if chat_state('A')['last'].startswith('Replay saved'):
        raise StepFailed("A's chat shows B's note")
    time.sleep(0.3)
    frame('B', 'online_save_note')
    ok(f"online SAVE: B's last shot saved as {os.path.basename(path)}, noted in B's chat only")
    n = int(replay_state('B')['saves'])                     # with A's ball in flight: after the shot
    send('A', 'move 400 420')
    time.sleep(0.3)
    click('A', 400, 420)
    wait(lambda: board('B')['state'] == '2', 10, "A's shot did not go")
    click('B', x, y)
    pending = replay_state('B')['pending'] == '1'
    wait(lambda: int(replay_state('B')['saves']) > n, 40, 'SAVE during the shot never saved')
    path = replay_state('B')['lastsaved']
    saved.append(path)
    if not os.path.basename(path).startswith("Panda's shot") or not replay_file_has_states(path):
        raise StepFailed(f"A's shot, saved by B, should be \"Panda's shot\": {path}")
    for c in 'AB':
        if state(c).get('desync') == '1':
            raise StepFailed('the games went out of sync with SAVE')
    send('B', 'move 400 300')                                # (SAVE lights up under B's pointer, on B's screen only)
    time.sleep(6.5)                                          # B's note fades
    synced('online_save')
    ok(f"online SAVE {'pressed in flight waited for the shot, ' if pending else ''}saved A's shot too; in sync")


def red_online():
    """Both copies: the joiner's RED PEG MODE box shows the host's and takes no clicks; an online duel's red peg is the
    same on both, and so are its effects when it's hit (Mirage, then Silhouette): pictures identical."""
    for k in 'AB':
        if not red_box(k):
            raise StepFailed(f'copy {k} has no RED PEG MODE box on the online selection')
    if chaos_box('A')[2]:
        x, y, _ = chaos_box('A')
        click('A', x, y)
    for want in (not red_box('A')[2], True):
        if red_box('A')[2] != want:
            x, y, _ = red_box('A')
            click('A', x, y)
        wait(lambda: red_box('A')[2] == want and red_box('B')[2] == want, 5,
             f"the joiner's RED PEG MODE box did not follow the host's ({want})")
    x, y, _ = red_box('B')
    click('B', x, y)
    time.sleep(0.8)
    if not red_box('B')[2]:
        raise StepFailed("the joiner's RED PEG MODE box should stay as the host has it")
    frame('B', 'red_online_selection')
    ok("online selection: the joiner's RED PEG MODE box shows the host's choice")
    duel()
    wait(lambda: all(chaos_state(c)['running'] == '1' for c in 'AB'), 30, 'the online duel never began')
    a, b = chaos_state('A'), chaos_state('B')
    fa, fb = chaos_fx('A'), chaos_fx('B')
    if a['red'] != '1' or b['red'] != '1' or a['on'] != '0' or fa['redpeg'] != fb['redpeg'] or fa['redpeg'] == '-1':
        raise StepFailed(f'both should have Red Peg Mode (no Chaos Mode) and the same red peg: A {a} {fa}, B {b} {fb}')
    synced('red_online_peg')
    glitches0 = {c: int(chaos_fx(c)['glitches']) for c in 'AB'}  # (counted since each copy started)
    for effect in (16, 17):
        for attempt in range(4):
            k = wait(shooter, 60, 'no one gets to shoot')
            for c in 'AB':
                send(c, f'chaos rednext {effect}')
            at = max(int(send(c, 'tick')) for c in 'AB') + 50
            since = mark()
            for c in 'AB':
                send(c, f'chaos redat 400 160 {at}')
            for c in 'AB':
                wait_log(c, 'the red peg is moved', since, 15, f'copy {c}: the red peg was not moved')
            rx, ry = fx_point(chaos_fx(k), 'redat')
            send(k, f'move {rx} {ry}')
            time.sleep(0.4)
            click(k, rx, ry)
            try:
                for c in 'AB':
                    wait_log(c, 'hit the red peg', since, 15, '')
                break
            except StepFailed:
                print(f'  copy {k} missed the red peg: again', flush=True)
                wait(lambda: shooter() not in (None, k), 60, 'the turn never passed')
        else:
            raise StepFailed('four shots at the red peg and none hit it')
        wait(lambda: all(chaos_state(c)['redeffect'] == str(effect) for c in 'AB'), 40,
             f'{RED_EFFECTS[effect]} did not begin on both')
        v = wait(shooter, 30, 'the other player never got to shoot')
        if v == k:
            raise StepFailed(f'copy {k} hit the red peg, so the other copy should get {RED_EFFECTS[effect]}')
        if effect == 16:
            fa, fb = chaos_fx('A'), chaos_fx('B')
            if fa['fakelist'] != fb['fakelist'] or fa['fakesinplaces'] != fb['fakesinplaces']:
                raise StepFailed(f'Mirage: different fake pegs: A {fa["fakelist"]} ({fa["fakesinplaces"]} in gone '
                                 f'pegs\' places), B {fb["fakelist"]} ({fb["fakesinplaces"]})')
        synced(f'red_online_{effect}')
        if effect == 16:                                     # through a fake peg (if no real one is in the way)
            fx, fy = fx_point(chaos_fx(v), 'fakeat')
            send(v, f'move {fx} {fy}')
            time.sleep(0.4)
            click(v, fx, fy)
        else:
            autoplay(1, seed=31)
        wait(lambda: all(chaos_state(c)['redeffect'] == '-1' for c in 'AB'), 40,
             f'{RED_EFFECTS[effect]} did not end on both after the turn')
        for c in 'AB':
            if state(c).get('desync') == '1':
                raise StepFailed(f'the games went out of sync ({RED_EFFECTS[effect]})')
        ga, gb = (int(chaos_fx(c)['glitches']) - glitches0[c] for c in 'AB')
        if ga != gb:
            raise StepFailed(f'the two games saw different glitches: A {ga}, B {gb}')
        synced(f'red_online_{effect}_after')
        ok(f'online: {RED_EFFECTS[effect]} the same on both computers (glitches {ga})')
    saved = []
    try:
        online_save(saved)
        since = mark()
        leave_board('B')
        wait_log('A', 'left the game', since, 10, 'copy A was not told that B left')
        time.sleep(3)
        for k in 'BA':                                       # the saved shot from the main menu's Replays
            if dialog_open(k, DLG_NOTICE):
                click(k, *(button_center(k, 'OK') or button_center(k, 'Ok')))
                time.sleep(2)
            if button_center(k, 'Replays'):
                replays_viewer(k, f"copy {k}: Dad's saved shot", saved[0])
                break
        else:
            raise StepFailed('neither copy shows Replays on its main menu')
    finally:
        for p in saved:
            if os.path.exists(p):
                os.remove(p)
    ok('Red Peg Mode (online) passed')


def main(argv):
    cmd = argv[0] if argv else 'full'
    try:
        if cmd == 'start':
            start()
        elif cmd == 'connect':
            connect()
        elif cmd == 'duel':
            duel()
        elif cmd == 'autoplay':
            autoplay(int(argv[1]), int(argv[2]) if len(argv) > 2 else 1)
        elif cmd == 'synced':
            synced(argv[1] if len(argv) > 1 else 'synced')
        elif cmd == 'chat':
            chat('chat', 0, 3)
            chat_click_away()
        elif cmd == 'localfirst':
            localfirst()
        elif cmd == 'scenario':
            scenario()
        elif cmd == 'full':
            start()
            localfirst()
            connect()
            duel()
            scenario()
            ok('full scenario passed')
        elif cmd == 'pad':
            padscenario()
        elif cmd == 'padfull':
            start()
            padscenario()
        elif cmd == 'local':
            padlocal()
        elif cmd == 'replays':
            pad_replays()
        elif cmd == 'display':
            displayscenario()
        elif cmd == 'displayfull':
            start()
            displayscenario()
        elif cmd == 'localfull':
            start()
            padlocal()
        elif cmd == 'chaos':
            chaoslocal()
        elif cmd == 'chaosfull':
            start()
            chaoslocal()
        elif cmd == 'chaosonline':
            connect()
            chaos_online()
        elif cmd == 'chaosonlinefull':
            start()
            connect()
            chaos_online()
        elif cmd in ('chaosreplay', 'chaosreplayfull'):
            if cmd == 'chaosreplayfull':
                start()
            chaos_local_start('A', check_box=False) if ini_value('duel', 'chaos') == '1' else chaos_local_start('A')
            effects = [int(e) for e in argv[1:]] or list(range(16))
            path = chaos_replays('A', effects)
            chaos_leave('A')
            try:
                time.sleep(2)
                replays_viewer('A', 'the saved shot', path, saved_upside_down('A') if 13 in effects else None)
            finally:
                if path and os.path.exists(path):
                    os.remove(path)
            ok('Instant Replay with Chaos Mode passed')
        elif cmd == 'chaoszen':
            chaos_local_start('A', check_box=False) if ini_value('duel', 'chaos') == '1' else chaos_local_start('A')
            chaos_rounds('A')
            chaos_zen('A')
            chaos_leave('A')
        elif cmd in ('chaospowers', 'chaospowersfull'):
            if cmd == 'chaospowersfull':
                start()
            chaos_local_start('A', check_box=False) if ini_value('duel', 'chaos') == '1' else chaos_local_start('A')
            chaos_powers('A')
            chaos_leave('A')
        elif cmd == 'redturning':
            red_brick_turning('A')
            chaos_leave('A')
            time.sleep(3)
            level_screen_home('A')
        elif cmd == 'redpeg':
            redpeg_local()
        elif cmd == 'redpegfull':
            start()
            redpeg_local()
        elif cmd == 'redpegonline':
            connect()
            red_online()
        elif cmd == 'redpegonlinefull':
            start()
            connect()
            red_online()
        elif cmd == 'finish':
            finish()
        elif cmd == 'shot':
            for k in 'AB':
                frame(k, argv[1] if len(argv) > 1 else 'duo')
        elif cmd == 'click':
            click(argv[1], argv[2], argv[3])
        elif cmd == 'settext':
            send(argv[1], f'settext {argv[2]} ' + ' '.join(argv[3:]))
        elif cmd == 'state':
            for k in 'AB':
                print(k, send(k, 'state'))
        elif cmd == 'log':
            print('\n'.join(log_lines(argv[1])[-(int(argv[2]) if len(argv) > 2 else 20):]))
        else:
            raise SystemExit(__doc__)
    except StepFailed as e:
        print('FAILED:', e, flush=True)
        for k in 'AB':
            try:
                frame(k, 'failed')
            except Exception:
                pass
        print(f'  screenshots: {SHOTS}\\failed_A.png, failed_B.png', flush=True)
        sys.exit(1)


if __name__ == '__main__':
    main(sys.argv[1:])
