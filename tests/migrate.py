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
from subprocess import Popen, PIPE
import os
import time
import socket
import datetime
import atexit

QMP_1, QMP_2 = "/tmp/migrate_test.1.qmp", "/tmp/migrate_test.2.qmp"
SPICE_PORT_1, SPICE_PORT_2 = 5911, 6911
MIGRATE_PORT = 9000
SPICEC_COUNT = 1
QEMU = "qemu.upstream"
LOG_FILENAME = "migrate_log.log"
IMAGE = "/store/images/F14_CCID.testing.qcow2"

qemu_exec = os.popen("which %s" % QEMU).read().strip()

def start_qemu(spice_port, qmp_filename, incoming_port=None):
    incoming_args = []
    if incoming_port:
        incoming_args = ("-incoming tcp::%s" % incoming_port).split()
    args = ([qemu_exec, "-qmp", "unix:%s,server,nowait" % qmp_filename,
        "-spice", "disable-ticketing,port=%s" % spice_port]
        + incoming_args)
    if os.path.exists(IMAGE):
        args += ["-m", "512", "-drive",
                 "file=%s,index=0,media=disk,cache=unsafe" % IMAGE, "-snapshot"]
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

def start_spicec(spice_port):
    return Popen(("spicec -h localhost -p %s" % spice_port).split(), executable="spicec")

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

    def __init__(self, log, monitor_files, spicec_count, spice_ports, migration_port):
        self.log = log
        self.migration_port = migration_port
        self.spicec_count = spicec_count
        self.monitor_files = monitor_files
        self.spice_ports = spice_ports
        self.active = start_qemu(spice_port=SPICE_PORT_1, qmp_filename=QMP_1)
        self.target = start_qemu(spice_port=SPICE_PORT_2, qmp_filename=QMP_2,
                            incoming_port=MIGRATE_PORT)
        self.remove_monitor_files()
        self.spicec = []

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
        if len(self.spicec) == 0:
            for i in range(self.spicec_count):
                self.spicec.append(start_spicec(spice_port=SPICE_PORT_1))
                wait_for_event(self.active.qmp, 'SPICE_INITIALIZED')
            if wait_for_user_input:
                print "waiting for Enter to start migrations"
                raw_input()
        self.active.qmp.cmd('client_migrate_info', {'protocol':'spice',
            'hostname':'localhost', 'port':self.target.spice_port})
        self.active.qmp.cmd('migrate', {'uri': 'tcp:localhost:%s' % MIGRATE_PORT})
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
                            qmp_filename=new_qmp_filename,
                            incoming_port=MIGRATE_PORT)
        print self.migration_count
        self.migration_count += 1

def main():
    log = open(LOG_FILENAME, "a+")
    log.write("# "+str(datetime.datetime.now())+"\n")

    migrator = Migrator(log = log, monitor_files = [QMP_1, QMP_2], migration_port = MIGRATE_PORT,
        spice_ports = [SPICE_PORT_1, SPICE_PORT_2], spicec_count = SPICEC_COUNT)
    atexit.register(cleanup, migrator)
    while True:
        migrator.iterate()

if __name__ == '__main__':
    main()

