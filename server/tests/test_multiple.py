#!/usr/bin/python

"""
Example usage:
./test_multiple.py test_display_no_ssl --log test.log

Resulting test.log file (single test, stopped with Ctrl-C with 15 clients):

0 162212
1 154156
2 154424
3 154952
4 155352
5 155616
6 156280
7 222084
8 222612
9 230820
10 230820
11 230820
12 230820
13 296356
14 296356

"""

import argparse
import os
import sys
import subprocess
import atexit
import socket
import time

def killall(procs):
    for p in procs:
        print "killing %d" % p.pid
        p.kill()
        p.wait()

def cleanup():
    killall(clients + [test_process])

def wait_for_port(port):
    if not port:
        return
    # TODO: do this without actually opening the port - maybe just look at /proc/qemu_process_id/fd?
    s = socket.socket(socket.AF_INET)
    while True:
        try:
            s.connect(('localhost', port))
            s.close()
            break
        except:
            time.sleep(1)
            pass

def get_vm_size(pid):
    """ read from /proc/<pid>/status, VmSize, in KiloBytes  """
    return int([x for x in open('/proc/%s/status' % pid).readlines() if 'VmSize' in x][0].split()[1])

parser = argparse.ArgumentParser()
parser.add_argument('--client', default='spicy')
parser.add_argument('--start-count', default=1, type=int)
parser.add_argument('--end-count', default=50, type=int)
parser.add_argument('--log', default='-')
parser.add_argument('--sleep', default=3, type=int)
args, rest = parser.parse_known_args(sys.argv[1:])
client = os.popen('which %s' % args.client).read().strip()
if not os.path.exists(client):
    print "supply a valid client. %s does not exist" % (args.client)
    sys.exit(1)

if not rest or len(rest) < 1 or not os.path.exists(rest[0]):
    print "supply one argument that is the tester you wish to run"
    sys.exit(1)

prog = rest[0]
port = {
'test_display_no_ssl': 5912,
'test_display_streaming': 5912,
'test_just_sockets_no_ssl': 5912,
'test_playback': 5701,
}.get(prog, None)

if args.log == '-':
    log = sys.stdout
else:
    log = open(args.log, 'a+')

log.write('#%s\n' % time.time())

# kill leftovers from previous invocation
os.system('killall lt-%s' % prog)

if prog[0] != '/':
    prog = os.path.join('.', prog)

if not port:
    print "unknown port for %r" % prog

print "prog = %r" % prog
print "client = %r" % client
print "range = %d..%d" % (args.start_count, args.end_count)
atexit.register(cleanup)
os.environ['SPICE_DEBUG_ALLOW_MC'] = '1'
test_process = subprocess.Popen([prog], executable=prog)
wait_for_port(port)
for count in xrange(args.start_count, args.end_count):
    print "starting %d clients" % count
    clients = [subprocess.Popen(args=[client, '-h', 'localhost', '-p', str(port)],
                                executable=client) for i in xrange(count)]
    print "sleeping %d" % (args.sleep * count)
    time.sleep(args.sleep * count)
    vmsize = "%d %d" % (i, get_vm_size(test_process.pid))
    print vmsize
    log.write(vmsize + '\n')
    log.flush()
    killall(clients)

test_process.wait()
