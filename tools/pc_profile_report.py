import argparse
import collections
import os
import subprocess

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'


def parse(path):
    threads = []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('thread '):
            parts = [p.strip() for p in line[len('thread '):].split('|')]
            info = {'name': parts[0], 'stacks': []}
            for p in parts[1:]:
                key, _, value = p.partition(' ')
                info[key] = value
            threads.append(info)
        elif line and line[0].isdigit() and threads:
            count, *frames = line.split(' ')
            threads[-1]['stacks'].append((int(count), frames))
    return threads


def symbolize(addresses, exe):
    pass
    names = {a: a for a in addresses}
    ours = sorted({a for a in addresses if a.lower().startswith('bo2zm.exe+')})
    if not ours or not os.path.exists(SYMBOLIZER):
        return names
    text = '\n'.join(a.split('+', 1)[1] for a in ours) + '\n'
    out = subprocess.run([SYMBOLIZER, '--obj=' + exe, '--relative-address', '--functions=linkage', '--demangle',
                          '--no-inlines'], input=text, capture_output=True, text=True).stdout
    blocks = [b for b in out.split('\n\n') if b.strip()]
    for a, block in zip(ours, blocks):
        lines = block.strip().split('\n')
        name = lines[0] if lines else a
        if name in ('??', ''):
            name = a
        names[a] = name
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--exe', default=os.path.join(REPO, 'app', 'out', 'win', 'bo2zm.exe'))
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--thread', default=None)
    ap.add_argument('--callers', default=None)
    ap.add_argument('--all', action='store_true', help='keep the samples of blocked threads')
    args = ap.parse_args()
    threads = parse(args.profile)
    if args.thread:
        threads = [t for t in threads if args.thread.lower() in t['name'].lower()]
    addresses = {f for t in threads for _, frames in t['stacks'] for f in frames}
    names = symbolize(addresses, args.exe)
    def blocked(frames):


        leaf = frames[0].lower()
        return leaf.startswith('ntdll.dll+') or leaf.startswith('win32u.dll+')

    threads.sort(key=lambda t: -float(t.get('cpu_ms', 0)))
    for t in threads:
        everything = sum(c for c, _ in t['stacks'])
        if not args.all:
            t['stacks'] = [(c, f) for c, f in t['stacks'] if not blocked(f)]
        total = sum(c for c, _ in t['stacks'])
        if total == 0:
            continue
        print('== %s  cpu %.0f ms, %d running samples of %d' % (t['name'], float(t.get('cpu_ms', 0)), total,
                                                               everything))
        self_counts = collections.Counter()
        total_counts = collections.Counter()
        callers = collections.defaultdict(collections.Counter)
        for count, frames in t['stacks']:
            funcs = [names.get(f, f) for f in frames]
            self_counts[funcs[0]] += count
            for f in set(funcs):
                total_counts[f] += count
            if args.callers:
                for i, f in enumerate(funcs[:-1]):
                    if args.callers.lower() in f.lower():
                        callers[f][funcs[i + 1]] += count
        print('   self %   total %  function')
        shown = set()
        for f, c in self_counts.most_common(args.top):
            print('   %5.1f   %6.1f   %s' % (100.0 * c / total, 100.0 * total_counts[f] / total, f[:150]))
            shown.add(f)
        print('   -- most total (inclusive) --')
        for f, c in total_counts.most_common(args.top):
            if f in shown:
                continue
            print('   %5.1f   %6.1f   %s' % (100.0 * self_counts[f] / total, 100.0 * c / total, f[:150]))
        for f, counter in callers.items():
            print('   callers of %s:' % f[:120])
            for g, c in counter.most_common(12):
                print('      %5.1f %%  %s' % (100.0 * c / total, g[:140]))


main()
