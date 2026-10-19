#!/usr/bin/env python3
"""Metamorphic tests of the LLVM pass's choice of what to read.

The pass decides which values are roots, how far a tree is collected, where it
is cut and which candidate goes first; none of that is about the value a tree
computes, only about its shape. So the same value written in other shapes must
come out of the pass the same: what changes is only whether the choices still
find it. A case here is one value in a context, and its variants are the same
value reshaped:

  commute       the operands of every commutative operation swapped
  reassociate   a chain of one associative operation grouped the other way
  split-const   a product by a constant taken as two products
  shl-as-mul    a shift by a constant written as the product it is, and back
  neg-as-mul    `0 - x` written as `x * -1`
  mask-select   a `select` written as the masks it is
  fanout        an inner value read by something else as well
  freeze        the leaves behind a `freeze` rather than `!noundef`
  undef-leaves  the leaves neither frozen nor `!noundef`
  own-block     the step in a block of its own

A case's value may itself be written as an identity first (`--identities`: an
obfuscator's spellings that the pass is expected to read through - the long
negation, a product split by a sign, a term that cancels over a live value).
The context is a chain of steps that each read one more byte, so a tree at the
end reaches more leaves than `max-vars` and one in the middle does not.

Each variant is run through the pass under every setting and then the clean-up
a host runs after it (InstCombine, GVN, ADCE), and checked:

  sound     it computes what the variant computed, on random inputs (`lli`);
  agrees    it is as small as the plain case simplified the same way: a shape
            or an identity that costs instructions is a choice that failed.

    mba_harness.py --plugin <CobraPass.so> --opt <opt> --lli <lli> [-j N]

prints, per setting, how many variants agree and the time it took, then each
transform's failures - the shapes the pass's choices do not survive - and each
case where two settings disagree. Exit status 1 when anything is unsound.
"""
import argparse
import concurrent.futures
import itertools
import os
import random
import re
import subprocess
import sys
import threading
import time

PRIME = 1099511628211
BASIS = 0xCBF29CE484222325
M64 = (1 << 64) - 1


def s64(value):
    value &= M64
    return value - (1 << 64) if value >> 63 else value


# --- Expressions ------------------------------------------------------------------------

class Node:
    """An i64 operation (or i1 for a comparison) over other nodes."""

    def __init__(self, op, *args, value=None, pred=None, width=64):
        self.op, self.args, self.value, self.pred, self.width = op, list(args), value, pred, width


def const(k):
    return Node('const', value=s64(k))


def leaf(name):
    return Node('leaf', value=name)


def op(name, a, b):
    return Node(name, a, b)


COMMUTATIVE = {'add', 'mul', 'and', 'or', 'xor'}


# The values a step computes from the hash so far and one byte.
def step_plain(h, e):
    return op('mul', op('xor', h, e), const(PRIME))


# Identities: the same step another way.
def identity_neg_long(h, e):
    x = op('xor', h, e)
    y = op('mul', x, const(-PRIME))
    return op('sub', op('or', op('shl', op('xor', y, const(-1)), const(1)), const(2)), op('xor', y, const(-2)))


def identity_sign_split(h, e):
    x = op('xor', h, e)
    negative = Node('icmp', x, const(0), pred='slt')
    return Node('select', negative, identity_neg_long(h, e), op('mul', x, const(PRIME)))


def identity_cancel(h, e, junk):
    x = op('xor', h, e)
    c = const(1640939767)
    zero = op('sub', op('sub', op('or', junk, c), op('and', junk, c)), op('xor', junk, c))
    return op('add', op('mul', x, const(PRIME)), zero)


def identity_xor_mba(h, e):
    x = op('sub', op('or', h, e), op('and', h, e))
    return op('mul', x, const(PRIME))


IDENTITIES = {
    'none': lambda h, e, junk: step_plain(h, e),
    'neg-long': lambda h, e, junk: identity_neg_long(h, e),
    'sign-split': lambda h, e, junk: identity_sign_split(h, e),
    'cancel': identity_cancel,
    'xor-mba': lambda h, e, junk: identity_xor_mba(h, e),
}


# --- Reshapings: the same value, another shape ---------------------------------------------

def walk(node, seen=None):
    seen = set() if seen is None else seen
    if node in seen:
        return
    seen.add(node)
    for a in node.args:
        yield from walk(a, seen)
    yield node


def reshape_commute(root, rng):
    for node in walk(root):
        if node.op in COMMUTATIVE:
            node.args.reverse()
    return root


def reshape_reassociate(root, rng):
    # (a o b) o c -> a o (b o c) wherever both are the same associative operation.
    for node in walk(root):
        if node.op in COMMUTATIVE and node.args[0].op == node.op:
            inner = node.args[0]
            a, b, c = inner.args[0], inner.args[1], node.args[1]
            node.args = [a, op(node.op, b, c)]
    return root


def inverse(k):
    x = k
    for _ in range(6):
        x = (x * (2 - k * x)) & M64
    return x


def reshape_split_const(root, rng):
    # x * k -> (x * a) * (k / a) for an odd `a`, where `k` is odd.
    for node in list(walk(root)):
        if node.op == 'mul' and node.args[1].op == 'const' and node.args[1].value & 1:
            k = node.args[1].value & M64
            a = rng.randrange(3, 1 << 20) | 1
            node.args = [op('mul', node.args[0], const(a)), const((k * inverse(a)) & M64)]
    return root


def reshape_shl_as_mul(root, rng):
    for node in walk(root):
        if node.op == 'shl' and node.args[1].op == 'const':
            node.op, node.args[1] = 'mul', const(1 << node.args[1].value)
        elif node.op == 'mul' and node.args[1].op == 'const' and node.args[1].value in (2, 4, 8, 16):
            node.op, node.args[1] = 'shl', const(node.args[1].value.bit_length() - 1)
    return root


def reshape_neg_as_mul(root, rng):
    for node in walk(root):
        if node.op == 'sub' and node.args[0].op == 'const' and node.args[0].value == 0:
            node.op, node.args = 'mul', [node.args[1], const(-1)]
        elif node.op == 'xor' and node.args[1].op == 'const' and node.args[1].value == -1:
            # ~x as -x - 1
            node.op, node.args = 'sub', [op('mul', node.args[0], const(-1)), const(1)]
    return root


def reshape_mask_select(root, rng):
    for node in walk(root):
        if node.op == 'select':
            condition, a, b = node.args
            m = Node('sext', condition)
            node.op, node.args = 'or', [op('and', a, m), op('and', b, op('xor', m, const(-1)))]
    return root


RESHAPES = {
    'commute': reshape_commute,
    'reassociate': reshape_reassociate,
    'split-const': reshape_split_const,
    'shl-as-mul': reshape_shl_as_mul,
    'neg-as-mul': reshape_neg_as_mul,
    'mask-select': reshape_mask_select,
}
# Reshapings of the context rather than of the expression.
CONTEXT = ('fanout', 'freeze', 'undef-leaves', 'own-block')


# --- Emission --------------------------------------------------------------------------------

class Emitter:
    def __init__(self):
        self.lines, self.count, self.names = [], 0, {}

    def name(self):
        self.count += 1
        return f'%v{self.count}'

    def emit(self, node):
        if node in self.names:
            return self.names[node]
        if node.op == 'const':
            return str(node.value)
        if node.op == 'leaf':
            return node.value
        args = [self.emit(a) for a in node.args]
        result = self.name()
        if node.op == 'icmp':
            self.lines.append(f'  {result} = icmp {node.pred} i64 {args[0]}, {args[1]}')
        elif node.op == 'select':
            self.lines.append(f'  {result} = select i1 {args[0]}, i64 {args[1]}, i64 {args[2]}')
        elif node.op == 'sext':
            self.lines.append(f'  {result} = sext i1 {args[0]} to i64')
        else:
            self.lines.append(f'  {result} = {node.op} i64 {args[0]}, {args[1]}')
        self.names[node] = result
        return result


def build(identity, length, position, reshapes, rng):
    """`length` steps over a byte buffer, the step at `position` written with
    `identity` and reshaped by `reshapes`."""
    e = Emitter()
    blocks = [('entry', e.lines)]
    h = const(BASIS)
    junk = None
    stored = []
    for step in range(length):
        p = e.name()
        b = e.name()
        x = e.name()
        attributes = '' if 'undef-leaves' in reshapes or 'freeze' in reshapes else ', !noundef !0'
        e.lines += [f'  {p} = getelementptr i8, ptr %bytes, i64 {step}',
                    f'  {b} = load i8, ptr {p}, align 1{attributes}',
                    f'  {x} = sext i8 {b} to i64']
        if 'freeze' in reshapes:
            frozen = e.name()
            e.lines.append(f'  {frozen} = freeze i64 {x}')
            x = frozen
        byte = leaf(x)
        junk = junk or byte
        if step == position:
            if 'own-block' in reshapes:
                label = f'step{step}'
                e.lines.append(f'  br label %{label}')
                e.lines = []
                blocks.append((label, e.lines))
            tree = IDENTITIES[identity](h, byte, junk)
            for name in reshapes:
                if name in RESHAPES:
                    tree = RESHAPES[name](tree, rng)
            if 'fanout' in reshapes:
                inner = [n for n in walk(tree) if n.op not in ('const', 'leaf', 'icmp') and n is not tree]
                if inner:
                    stored.append(inner[len(inner) // 2])
            h = tree
        else:
            h = step_plain(h, byte)
        h = leaf(e.emit(h))
        for node in stored:
            if not isinstance(node, str):
                stored[stored.index(node)] = e.emit(node)
    stores = [f'  store i64 {v}, ptr %out, align 8' for v in stored]
    e.lines += stores + [f'  ret i64 {h.value}']
    body = '\n'.join(f'{label}:\n' + '\n'.join(lines) for label, lines in blocks)
    return f'define i64 @f(ptr %bytes, ptr %out) {{\n{body}\n}}\n\n!0 = !{{}}\n', len(stores)


DRIVER = '''
@buffer = global [32 x i8] zeroinitializer
@sink = global i64 0
@format = private constant [6 x i8] c"%llx\\0A\\00"
declare i32 @printf(ptr, ...)

define i32 @main() {
entry:
  br label %loop
loop:
  %seed = phi i64 [ 88172645463325252, %entry ], [ %next, %done ]
  %round = phi i32 [ 0, %entry ], [ %round.next, %done ]
  br label %fill
fill:
  %i = phi i64 [ 0, %loop ], [ %i.next, %fill ]
  %s = phi i64 [ %seed, %loop ], [ %s3, %fill ]
  %s1 = shl i64 %s, 13
  %t1 = xor i64 %s, %s1
  %s2 = lshr i64 %t1, 7
  %t2 = xor i64 %t1, %s2
  %s4 = shl i64 %t2, 17
  %s3 = xor i64 %t2, %s4
  %byte = trunc i64 %s3 to i8
  %slot = getelementptr i8, ptr @buffer, i64 %i
  store i8 %byte, ptr %slot, align 1
  %i.next = add i64 %i, 1
  %full = icmp eq i64 %i.next, 32
  br i1 %full, label %call, label %fill
call:
  store i64 0, ptr @sink, align 8
  %r = call i64 @f(ptr @buffer, ptr @sink)
  %stored = load i64, ptr @sink, align 8
  %both = xor i64 %r, %stored
  %printed = call i32 (ptr, ...) @printf(ptr @format, i64 %both)
  br label %done
done:
  %next = add i64 %s3, 1442695040888963407
  %round.next = add i32 %round, 1
  %more = icmp ult i32 %round.next, 64
  br i1 %more, label %loop, label %exit
exit:
  ret i32 0
}
'''


def instructions(text):
    """The instructions the returned value is computed by: a value something else
    reads as well (`fanout`) stays for that reader, and is not counted for it."""
    body = text.split('define i64 @f(', 1)[1].split('\n}', 1)[0]
    defs, returned = {}, None
    for line in body.split('\n'):
        line = line.split(', !')[0]
        if line.startswith('  ret i64 '):
            returned = line.split()[-1]
        elif line.startswith('  %') and ' = ' in line:
            name, rhs = line.strip().split(' = ', 1)
            defs[name] = re.findall(r'%[\w.]+', rhs)
    seen, pending = set(), [returned]
    while pending:
        value = pending.pop()
        if value in defs and value not in seen:
            seen.add(value)
            pending.extend(defs[value])
    return len(seen)


def run(command, stdin):
    started = time.monotonic()
    result = subprocess.run(command, input=stdin, capture_output=True, text=True)
    return result, time.monotonic() - started


class Harness:
    def __init__(self, args):
        self.args = args
        self.baselines = {}
        self.lock = threading.Lock()

    def simplify(self, source, setting):
        pipeline = f'function(cobra-simplify<{setting}>,instcombine,gvn,instcombine,adce,simplifycfg)'
        result, seconds = run([self.args.opt, f'-load-pass-plugin={self.args.plugin}', f'-passes={pipeline}',
                               '-S', '-'], source)
        if result.returncode != 0:
            raise RuntimeError(f'opt failed: {result.stderr[-2000:]}')
        return result.stdout, seconds

    def outputs(self, module):
        result, _ = run([self.args.lli, '-'], module + DRIVER)
        if result.returncode != 0:
            raise RuntimeError(f'lli failed: {result.stderr[-2000:]}')
        return result.stdout

    def baseline(self, length, position, setting, context):
        """The plain step in the same context, simplified the same way."""
        key = (length, position, setting, context)
        with self.lock:
            if key in self.baselines:
                return self.baselines[key]
        source, extra = build('none', length, position, context, random.Random(0))
        simplified, _ = self.simplify(source, setting)
        size = instructions(simplified)
        with self.lock:
            self.baselines[key] = size
        return size

    def variant(self, case):
        identity, length, position, reshapes, setting = case
        source, extra = build(identity, length, position, reshapes, random.Random(hash((identity, reshapes)) & 0xFFFF))
        simplified, seconds = self.simplify(source, setting)
        expected, got = self.outputs(source), self.outputs(simplified)
        context = tuple(r for r in reshapes if r in ('freeze', 'undef-leaves', 'own-block'))
        target = self.baseline(length, position, setting, context)
        size = instructions(simplified)
        if self.args.keep:
            stem = re.sub(r'[^a-z0-9]+', '-', f'{identity} {length} {position} {" ".join(reshapes)} {setting}'.lower())
            for suffix, text in (('in', source), ('out', simplified)):
                with open(os.path.join(self.args.keep, f'{stem.strip("-")}.{suffix}.ll'), 'w') as stream:
                    stream.write(text)
        return case, expected == got, size, target, seconds


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin', required=True)
    parser.add_argument('--opt', required=True)
    parser.add_argument('--lli', required=True)
    parser.add_argument('--settings', nargs='+',
                        default=['verified;max-vars=4;timeout=500',
                                 'verified;wide-roots;max-vars=4;timeout=500'])
    parser.add_argument('--identities', nargs='+', default=list(IDENTITIES))
    parser.add_argument('--lengths', nargs='+', type=int, default=[3, 6])
    parser.add_argument('--reshapes', nargs='+', default=list(RESHAPES) + list(CONTEXT))
    parser.add_argument('--pairs', action='store_true', help='also every two reshapings together')
    parser.add_argument('-j', type=int, default=4)
    parser.add_argument('--keep', default='')
    args = parser.parse_args()
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    shapes = [()] + [(r,) for r in args.reshapes]
    if args.pairs:
        shapes += [p for p in itertools.combinations(args.reshapes, 2)
                   if not {'freeze', 'undef-leaves'} <= set(p)]
    cases = []
    for identity, length in itertools.product(args.identities, args.lengths):
        for position in sorted({length // 2, length - 1}):
            for shape in shapes:
                for setting in args.settings:
                    cases.append((identity, length, position, shape, setting))

    harness = Harness(args)
    results = []
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        for case, sound, size, target, seconds in pool.map(harness.variant, cases):
            identity, length, position, shape, setting = case
            verdict = 'UNSOUND' if not sound else 'ok' if size <= target else 'left'
            print(f'{verdict:7s} {identity:10s} len={length} at={position} {"+".join(shape) or "-":24s} '
                  f'[{setting}] {size} vs {target}  {seconds:.1f} s', flush=True)
            results.append((case, sound, size, target, seconds))

    print(f'\n{len(results)} runs in {time.monotonic() - started:.0f} s')
    print('per setting: agree / variants, solver time')
    for setting in args.settings:
        mine = [r for r in results if r[0][4] == setting]
        print(f'  [{setting}] {sum(r[1] and r[2] <= r[3] for r in mine)} / {len(mine)}, '
              f'{sum(r[4] for r in mine):.0f} s')
    print('per reshaping and setting: variants left larger than the plain case')
    for shape in shapes:
        line = []
        for setting in args.settings:
            mine = [r for r in results if r[0][3] == shape and r[0][4] == setting]
            line.append(f'{sum(r[2] > r[3] for r in mine)}/{len(mine)}')
        print(f'  {"+".join(shape) or "(as written)":28s} ' + '  '.join(line))
    by_case = {}
    for (identity, length, position, shape, setting), sound, size, target, _ in results:
        by_case.setdefault((identity, length, position, shape), {})[setting] = size
    differ = [(k, v) for k, v in by_case.items() if len(set(v.values())) > 1]
    print(f'cases the settings answer differently: {len(differ)}')
    for (identity, length, position, shape), sizes in differ[:40]:
        print(f'  {identity} len={length} at={position} {"+".join(shape) or "-"}: '
              + ', '.join(f'{s.split(";")[1] if ";" in s else s}={n}' for s, n in sizes.items()))
    unsound = sum(not r[1] for r in results)
    print(f'unsound: {unsound}')
    return 1 if unsound else 0


if __name__ == '__main__':
    sys.exit(main())
