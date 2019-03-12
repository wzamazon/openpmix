#!/opt/local/bin/python

from pmix import *
import signal, time
import os
import select
import subprocess

global killer

class GracefulKiller:
  kill_now = False
  def __init__(self):
    signal.signal(signal.SIGINT, self.exit_gracefully)
    signal.signal(signal.SIGTERM, self.exit_gracefully)

  def exit_gracefully(self,signum, frame):
    self.kill_now = True

def clientconnected(args:dict is not None):
    print("CLIENT CONNECTED", args)
    return PMIX_SUCCESS

def main():
    try:
        foo = PMIxServer()
    except:
        print("FAILED TO CREATE SERVER")
        exit(1)
    print("Testing server version ", foo.get_version())
    args = {'FOOBAR': ('VAR', 'string'), 'BLAST': (7, 'size')}
    map = {'clientconnected': clientconnected}
    my_result = foo.init(args, map)
    print("Testing PMIx_Initialized")
    rc = foo.initialized()
    print("Initialized: ", rc)
    vers = foo.get_version()
    print("Version: ", vers)
    # get our environment as a base
    env = os.environ.copy()
    # register an nspace for the client app
    kvals = {}
    rc = foo.register_nspace("testnspace", 1, kvals)
    print("RegNspace ", rc)
    # register a client
    uid = os.getuid()
    gid = os.getgid()
    rc = foo.register_client(("testnspace", 0), uid, gid)
    print("RegClient ", rc)
    # setup the fork
    rc = foo.setup_fork(("testnspace", 0), env)
    print("SetupFrk", rc)
    # setup the client argv
    args = ["./client.py"]
    # open a subprocess with stdout and stderr
    # as distinct pipes so we can capture their
    # output as the process runs
    p = subprocess.Popen(args, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # define storage to catch the output
    stdout = []
    stderr = []
    # loop until the pipes close
    while True:
        reads = [p.stdout.fileno(), p.stderr.fileno()]
        ret = select.select(reads, [], [])

        stdout_done = True
        stderr_done = True

        for fd in ret[0]:
            # if the data
            if fd == p.stdout.fileno():
                read = p.stdout.readline()
                if read:
                    read = read.decode('utf-8').rstrip()
                    print('stdout: ' + read)
                    stdout_done = False
            elif fd == p.stderr.fileno():
                read = p.stderr.readline()
                if read:
                    read = read.decode('utf-8').rstrip()
                    print('stderr: ' + read)
                    stderr_done = False

        if stdout_done and stderr_done:
            break

    print("FINALIZING")
    foo.finalize()

if __name__ == '__main__':
    global killer
    killer = GracefulKiller()
    main()
