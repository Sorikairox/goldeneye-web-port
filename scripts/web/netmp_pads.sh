# G6: scripted pads for the multiplayer tests (sourced by scripts/web/netpair.sh; netdet.sh runs use the same PADn; webnet.c
# GE_NETPADn syntax, times in retraces from power-on, det mode). Player 1 drives the real menu route:
# boot -> A (skips the gun-barrel intro to the title) -> A (file select) -> A (file 1) -> mode select: stick down to the
# multiplayer entry, A -> multiplayer options (players = controllers present, Normal, Temple, 10 min) -> START ->
# the match. Then every player walks, turns and fires until the end (PLAY_FROM .. PLAY_FROM + PLAY_SECS x 50).
# Tunables: PLAY_SECS (default 120).
PLAY_FROM=${PLAY_FROM:-2300}
PLAY_SECS=${PLAY_SECS:-120}
MENU='1100:A;1300:A;1500:SDOWN;1508:SNONE;1600:A;1800:START;1900:START;2000:START'
play() {   # channel seed -> an entry list of PLAY_SECS seconds of moving, turning, firing (deterministic per channel)
  python3 - "$1" "$PLAY_FROM" "$PLAY_SECS" <<'EOF'
import random, sys
ch, start, secs = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
r = random.Random(1000 + ch)
out, f = [], start
moves = ['SUP', 'SUP', 'SDOWN', 'SLEFT', 'SRIGHT', 'SUP,CLEFTHOLD', 'SUP,CRIGHTHOLD', 'SX40,SY60', 'SX-40,SY60']
while f < start + secs * 50:
    m = r.choice(moves)
    if 'HOLD' not in m: m += ',CLEFTREL,CRIGHTREL,CUPREL'
    out.append(f'{f}:{m}')
    if r.random() < 0.5: out.append(f'{f + r.randint(5, 30)}:Z')
    if r.random() < 0.15: out.append(f'{f + r.randint(5, 30)}:A')
    f += r.randint(25, 90)
out.append(f'{f}:SNONE,CLEFTREL,CRIGHTREL,CUPREL')
print(';'.join(out))
EOF
}
PAD0="$MENU;$(play 0)"
PAD1="$(play 1)"
PAD2="$(play 2)"
PAD3="$(play 3)"
export PAD0 PAD1 PAD2 PAD3
