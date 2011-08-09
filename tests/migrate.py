#!/usr/bin/env python
"""
Spice Migration test

Somewhat stressfull test of continuous migration with spice in VGA mode or QXL mode,
depends on supplying an image in IMAGE variable (if no image is supplied then
VGA mode since it will just be SeaBIOS).

Dependencies:
either qmp in python path or running with spice and qemu side by side:
qemu/QMP/qmp.py
spice/tests/migrate.py

Will create two temporary unix sockets in /tmp
Will leave a log file, migrate_test.log, in current directory.
"""

#
# start one spiceclient, have two machines (active and target),
# and repeat:
#  active wait until it's active
#  active client_migrate_info
#  active migrate tcp:localhost:9000
#  _wait for event of quit
#  active stop, active<->passive
#
# wait until it's active
#  command query-status, if running good
#  if not listen to events until event of running

try:
    import qmp
except:
    import sys
    sys.path.append("../../qemu/QMP")
    try:
        import qmp
    except:
        print "can't find qmp"
        raise SystemExit
import sys
from subprocess import Popen, PIPE
import os
import time
import socket
import datetime
import atexit
import argparse

def get_args():
    parser = argparse.ArgumentParser(description='Process some integers.')
    parser.add_argument('--qmp1', dest='qmp1', default='/tmp/migrate_test.1.qmp')
    parser.add_argument('--qmp2', dest='qmp2', default='/tmp/migrate_test.2.qmp')
    parser.add_argument('--spice_port1', dest='spice_port1', type=int, default=5911)
    parser.add_argument('--spice_port2', dest='spice_port2', type=int, default=6911)
    parser.add_argument('--migrate_port', dest='migrate_port', type=int, default=8000)
    parser.add_argument('--client_count', dest='client_count', type=int, default=1)
    parser.add_argument('--qemu', dest='qemu', default='../../qemu/x86_64-softmmu/qemu-system-x86_64')
    parser.add_argument('--log_filename', dest='log_filename', default='migrate.log')
    parser.add_argument('--image', dest='image', default='')
    parser.add_argument('--client', dest='client', default='spicy', choices=['spicec', 'spicy'])
    args = parser.parse_args(sys.argv[1:])
    if os.path.exists(args.qemu):
        args.qemu_exec = args.qemu
    else:
        args.qemu_exec = os.popen("which %s" % args.qemu).read().strip()
    if not os.path.exists(args.qemu_exec):
        print "qemu not found (qemu = %r)" % args.qemu_exec
        sys.exit(1)
    return args

def start_qemu(qemu_exec, image, spice_port, qmp_filename, incoming_port=None):
    incoming_args = []
    if incoming_port:
        incoming_args = ("-incoming tcp::%s" % incoming_port).split()
    args = ([qemu_exec, "-qmp", "unix:%s,server,nowait" % qmp_filename,
        "-spice", "disable-ticketing,port=%s" % spice_port]
        + incoming_args)
    if os.path.exists(image):
        args += ["-m", "512", "-drive",
                 "file=%s,index=0,media=disk,cache=unsafe" % image, "-snapshot"]
    proc = Popen(args, executable=qemu_exec, stdin=PIPE, stdout=PIPE)
    while not os.path.exists(qmp_filename):
        time.sleep(0.1)
    proc.qmp_filename = qmp_filename
    proc.qmp = qmp.QEMUMonitorProtocol(qmp_filename)
    while True:
        try:
            proc.qmp.connect()
            break
        except socket.error, err:
            pass
    proc.spice_port = spice_port
    proc.incoming_port = incoming_port
    return proc

def start_client(client, spice_port):
    return Popen(("%(client)s -h localhost -p %(port)d" % dict(port=spice_port,
        client=client)).split(), executable=client)

def wait_active(q, active):
    events = ["RESUME"] if active else ["STOP"]
    while True:
        try:
            ret = q.cmd("query-status")
        except:
            # ValueError
            time.sleep(0.1)
            continue
        if ret and ret.has_key("return"):
            if ret["return"]["running"] == active:
                break
        for e in q.get_events():
            if e["event"] in events:
                break
        time.sleep(0.5)

def wait_for_event(q, event):
    while True:
        for e in q.get_events():
            if e["event"] == event:
                return
        time.sleep(0.5)

def cleanup(migrator):
    print "doing cleanup"
    migrator.close()

class Migrator(object):

    migration_count = 0

    def __init__(self, log, client, qemu_exec, image, monitor_files, client_count,
                 spice_ports, migration_port):
        self.client = client
        self.log = log
        self.qemu_exec = qemu_exec
        self.image = image
        self.migration_port = migration_port
        self.client_count = client_count
        self.monitor_files = monitor_files
        self.spice_ports = spice_ports
        self.active = start_qemu(qemu_exec=qemu_exec, image=image, spice_port=spice_ports[0],
                                 qmp_filename=monitor_files[0])
        self.target = start_qemu(qemu_exec=qemu_exec, image=image, spice_port=spice_ports[1],
                                 qmp_filename=monitor_files[1], incoming_port=migration_port)
        self.remove_monitor_files()
        self.clients = []

    def close(self):
        self.remove_monitor_files()
        self.kill_qemu()

    def kill_qemu(self):
        for p in [self.active, self.target]:
            print "killing and waiting for qemu pid %s" % p.pid
            p.kill()
            p.wait()

    def remove_monitor_files(self):
        for x in self.monitor_files:
            if os.path.exists(x):
                os.unlink(x)

    def iterate(self, wait_for_user_input=False):
        wait_active(self.active.qmp, True)
        wait_active(self.target.qmp, False)
        if len(self.clients) == 0:
            for i in range(self.client_count):
                self.clients.append(start_client(client=self.client,
                    spice_port=self.spice_ports[0]))
                wait_for_event(self.active.qmp, 'SPICE_INITIALIZED')
            if wait_for_user_input:
                print "waiting for Enter to start migrations"
                raw_input()
        self.active.qmp.cmd('client_migrate_info', {'protocol':'spice',
            'hostname':'localhost', 'port':self.target.spice_port})
        self.active.qmp.cmd('migrate', {'uri': 'tcp:localhost:%s' % self.migration_port})
        wait_active(self.active.qmp, False)
        wait_active(self.target.qmp, True)
        wait_for_event(self.target.qmp, 'SPICE_CONNECTED')
        dead = self.active
        dead.qmp.cmd("quit")
        dead.qmp.close()
        dead.wait()
        new_spice_port = dead.spice_port
        new_qmp_filename = dead.qmp_filename
        self.log.write("# STDOUT dead %s\n" % dead.pid)
        self.log.write(dead.stdout.read())
        del dead
        self.active = self.target
        self.target = start_qemu(spice_port=new_spice_port,
                            qemu_exec=self.qemu_exec, image=self.image,
                            qmp_filename=new_qmp_filename,
                            incoming_port=self.migration_port)
        print self.migration_count
        self.migration_count += 1

def main():
    args = get_args()
    print "log file %s" % args.log_filename
    log = open(args.log_filename, "a+")
    log.write("# "+str(datetime.datetime.now())+"\n")
    migrator = Migrator(client=args.client, qemu_exec=args.qemu_exec,
        image=args.image, log=log, monitor_files=[args.qmp1, args.qmp2],
        migration_port=args.migrate_port, spice_ports=[args.spice_port1,
        args.spice_port2], client_count=args.client_count)
    atexit.register(cleanup, migrator)
    while True:
        migrator.iterate()

if __name__ == '__main__':
    main()
