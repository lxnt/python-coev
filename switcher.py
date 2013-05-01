#!/usr/bin/python
# -*- encoding: utf-8 -*-

import argparse, traceback

"""
a synthetic benchmark - measures the number of coroutine switches 
done in given time, given some arbitrary python stack depth (achieved by simple recursion).

tests gevent or coev, depending on which one is available.

"""

SWITCH_COUNT = 0

def stack_filler(depth, stall):
    global SWITCH_COUNT
    if depth > 0:
        stack_filler(depth - 1, stall)
    else:
        while True:
            SWITCH_COUNT += 1
            stall()
            
def test_coev(num, depth, period, settle):
    global SWITCH_COUNT
    horde = []
    for x in xrange(num):
        horde.append(thread.start_new_thread(stack_filler, (depth, coev.stall)))
    print("started {} coevs, settling".format(num))
    coev.sleep(settle)
    print("settle period over, {:.2f} sw/sec, testing".format(SWITCH_COUNT/(1.0*settle)))
    SWITCH_COUNT = 0
    coev.sleep(period)
    print("testing period over, {:.2f} sw/sec".format(SWITCH_COUNT/(1.0*period)))
    for tid in horde:
        try:
            coev.throw(tid, SystemExit)
        except:
            pass
    coev.unloop()

def test_gevent(num, depth, period, settle):
    global SWITCH_COUNT

    import gevent    
    from gevent.pool import Group
    from gevent.hub import sleep
    horde = Group()
    for x in xrange(num):
        horde.spawn(stack_filler, depth, sleep) 
    gevent.sleep(settle)
    print("settle period over, {:.2f} sw/sec, testing".format(SWITCH_COUNT/(1.0*settle)))
    SWITCH_COUNT=0
    gevent.sleep(period)
    print("testing period over, {:.2f} sw/sec".format(SWITCH_COUNT/(1.0*period)))
    horde.kill()

ap = argparse.ArgumentParser()
ap.add_argument('-depth', metavar='depth', type=int, help='recursion level/stack depth at switch', default=32)
ap.add_argument('-period', metavar='seconds', type=int, help='test period after settle-down', default=10)
ap.add_argument('-settle', metavar='seconds', type=int, help='settle period before measurement', default=2)
ap.add_argument('-num', metavar='coevs', type=int, help='number of coroutines to launch', default=512)
pa = ap.parse_args()

try:
    import coev, thread
    thread.start_new_thread(test_coev, (pa.num, pa.depth, pa.period, pa.settle))
    coev.scheduler()
except ImportError, cer:
    test_gevent(pa.num, pa.depth, pa.period, pa.settle)



