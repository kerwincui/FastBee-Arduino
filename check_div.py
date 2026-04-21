f = open('data/www/pages/protocol.html', 'r')
lines = f.readlines()
f.close()
depth = 0
for i in range(305, 680):
    opens = lines[i].count('<div')
    closes = lines[i].count('</div')
    prev = depth
    depth += opens - closes
    if depth < prev or i >= 665:
        print('L%d: depth %d->%d (+%d -%d) | %s' % (i+1, prev, depth, opens, closes, lines[i].strip()[:80]))
