#!/usr/bin/env python3
'''
Based on perf-prof command

perf-prof
    Profiling based on perf_event and ebpf
'''

import os
import re
import time
import stat
import shutil
import signal
import threading
import selectors
import subprocess
from threading import Timer
try:
    from collections.abc import Iterable
except ImportError:
    from collections import Iterable


PERF_PROF_PATH = shutil.which("perf-prof", path=os.getcwd() + ":" + os.getcwd() + "/..:" + os.environ.get("PATH"))
TCMALLOC = "/lib64/libtcmalloc.so"
TRACING = "/sys/kernel/debug/tracing/"

class PerfProf(object):
    '''
        PerfProf
        Usage
            1) Create PerfProf object
               oncpu = PerfProf(["oncpu", "-C", "0"])
            2) Append command line arguments
               oncpu += ["-m", "128"]
            3) Iterate command output
               for std, line in oncpu.run():
                   if std == PerfProf.STDOUT:
                       print("STDOUT: " + line, end='')
                   elif std == PerfProf.STDERR:
                       print("STDERR: " + line, end='')
    '''
    def __init__(self, args):
        PerfProf.STDOUT = 1
        PerfProf.STDERR = 2
        PerfProf.SIGUSR1 = signal.SIGUSR1
        PerfProf.SIGUSR2 = signal.SIGUSR2

        self.args = [PERF_PROF_PATH]
        self.args.extend(args)

        self.timer = None
        self.util_timer = None

    def __iadd__(self, arg):
        if isinstance(arg, Iterable):
            self.args.extend(arg)
        else:
            self.args.append(arg)
        return self

    def run(self, runtime=None, memleak_check=False, extra_args=[], input=None, util_interval=None, _args_print=True):
        env = None
        stdin = None

        if memleak_check == "valgrind":
            # Use valgrind for memory leak detection
            valgrind_args = ["valgrind", "--leak-check=full", "--track-origins=yes", "--error-exitcode=1"]
            # Create a copy of args to avoid modifying the original
            self.args = valgrind_args + self.args.copy()
        elif memleak_check:
            # Use tcmalloc for memory leak detection (existing functionality)
            env = {"LD_PRELOAD" : TCMALLOC,
                   "HEAPCHECK"  : "draconian",
                   "PPROF_PATH" : PERF_PROF_PATH}
        if input:
            stdin = subprocess.PIPE

        if _args_print:
            if runtime != None and runtime > 0:
                print('Runtime ' + str(runtime) + ' second', end='')
            print('\033[35m')
            if memleak_check == "valgrind":
                print('valgrind_check ', end='')
            elif memleak_check:
                for key, value in env.items():
                    print(key + '=' + value + ' ', end='')
            _a = list(map(self.escape_to_shell_param, self.args))
            _e = list(map(self.escape_to_shell_param, extra_args))
            print(' '.join(_a + _e) + '\033[0m')

        if runtime != None and runtime == 0:
            return

        self.perfprof = subprocess.Popen(self.args + extra_args,
                        env = env,
                        bufsize = 1,
                        stdin = stdin,
                        stdout = subprocess.PIPE,
                        stderr = subprocess.PIPE,
                        universal_newlines = True,
                        )

        if runtime != None:
            self.timer = Timer(runtime, self.terminate)
            self.timer.start()
        if util_interval != None:
            self.util_interval = util_interval
            self.util_timer = Timer(self.util_interval, self._util_timer_handler)
            self.util_timer.start()
        signal.signal(signal.SIGINT, self._signal_handler)

        try:
            with selectors.DefaultSelector() as sel:
                if input:
                    input_line = 0
                    input_lines = input.split('\n')
                    sel.register(self.perfprof.stdin, selectors.EVENT_WRITE)
                sel.register(self.perfprof.stdout, selectors.EVENT_READ)
                sel.register(self.perfprof.stderr, selectors.EVENT_READ)
                std = {self.perfprof.stdout: PerfProf.STDOUT,
                       self.perfprof.stderr: PerfProf.STDERR}

                while sel.get_map():
                    for key, event in sel.select():
                        if input and key.fileobj is self.perfprof.stdin:
                            line = input_lines[input_line]
                            try:
                                if input_line < len(input_lines)-1:
                                    line += '\n'
                                key.fileobj.write(line)
                                input_line += 1
                            except BrokenPipeError:
                                sel.unregister(key.fileobj)
                                key.fileobj.close()
                            else:
                                if input_line >= len(input_lines):
                                    sel.unregister(key.fileobj)
                                    key.fileobj.close()
                        else:
                            line = key.fileobj.readline()
                            if not line:
                                sel.unregister(key.fileobj)
                                continue

                            sig = yield (std[key.fileobj], line)
                            if sig == PerfProf.SIGUSR1:
                                self.perfprof.send_signal(signal.SIGUSR1)
                            elif sig == PerfProf.SIGUSR2:
                                self.perfprof.send_signal(signal.SIGUSR2)
        finally:
            self.terminate()

    def terminate(self):
        if self.timer != None:
            self.timer.cancel()
            self.timer = None
        if self.util_timer != None:
            self.util_timer.cancel()
            self.util_timer = None
        if self.perfprof.poll() == None:
            try:
                self.perfprof.send_signal(signal.SIGUSR2)
                self.perfprof.send_signal(signal.SIGINT)
                retcode = self.perfprof.wait(10)
                if retcode is not None and retcode < 0:
                    raise subprocess.CalledProcessError(retcode, PERF_PROF_PATH)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                self.perfprof.kill()


    def _signal_handler(self, signum, frame):
        self.terminate()

    def _util_timer_handler(self):
        self.perfprof.send_signal(signal.SIGUSR2)
        self.util_timer = Timer(self.util_interval, self._util_timer_handler)
        self.util_timer.start()

    def help(self):
        for std, line in self.run(extra_args=['help']):
            print(line, end='', flush=True)

    @staticmethod
    def escape_to_shell_param(s):
        token = ('"', '\\', '$', '>', '<', '!', '?', '~', '*', '@', '(', ')', '{', '}', '[', ']', '|', '&', ';', ' ')
        for t in token:
            if t in s:
                s = "'" + s + "'"
                return s
        return s

    @staticmethod
    def lost_events(line):
        # 2022-11-23 21:25:54.792888 trace: lost 6 events on CPU #24
        record = line.split()
        if len(record) >= 5 and record[3] == 'lost' and record[5] == 'events':
            return True
        else:
            return False

    @staticmethod
    def memleak(line, bytes):
        # Leak check _main_ detected leaks of 1745 bytes in 21 objects
        if line.startswith('Leak check'):
            record = line.split()
            if len(record) >= 11 and record[2] == '_main_' and int(record[6]) <= bytes:
                return False
            else:
                return True
        else:
            return False

    @staticmethod
    def profilers():
        profiler_start = False
        profilers = []
        perf_prof = PerfProf(['-h'])
        for std, line in perf_prof.run(_args_print=False):
            if line.find('Available Profilers:') >= 0:
                profiler_start = True
                continue
            if profiler_start:
                if line == '\n':
                    profiler_start = False
                    continue
                profiler = line.split(maxsplit=1)
                profilers.append(profiler[0])
        return profilers

    @staticmethod
    def kernel_release(match=None):
        release = os.uname().release
        if match != None:
            return release.find(match) != -1
        else:
            return release

    @staticmethod
    def gettid():
        machine = os.uname().machine
        if machine == 'x86_64':
            SYS_gettid = 186 # unistd_64.h: #define __NR_gettid 186
        elif machine == 'i386':
            SYS_gettid = 224 # unistd_32.h: #define __NR_gettid 224
        else:
            SYS_gettid = 178 # asm-generic/unistd.h: #define __NR_gettid 178
        import ctypes
        tid = ctypes.CDLL(None).syscall(SYS_gettid)
        return tid

    @staticmethod
    def sysctl(sys, value=None):
        path = '/proc/sys/' + sys.replace('.', '/')
        with open(path) as f:
            old = f.read()
        if value != None:
            with open(path, 'w') as f:
                f.write(value)
        return old

    @staticmethod
    def scan_block_devices(directory):
        block_devices = []

        for entry in os.scandir(directory):
            if stat.S_ISBLK(entry.stat().st_mode) and not entry.name.startswith('nbd'):
                block_devices.append(entry.name)

        return block_devices

    @staticmethod
    def kallsyms_lookup_name(name):
        with open("/proc/kallsyms") as syms:
            for line in syms:
                sym = line.split()
                if sym[2].strip() == name:
                    return int(sym[0], 16)

    @staticmethod
    def tracepoint_exists(*names):
        exists = []
        for name in names:
            tp = name.split(':', 1)
            if len(tp) != 2 or not os.path.exists(TRACING + "events/" + tp[0] + "/" + tp[1]):
                exists.append(False)
            else:
                exists.append(True)
        return False not in exists, tuple(exists)

    @staticmethod
    def event_format(name):
        tp = name.split(':', 1)
        if len(tp) != 2:
            return None

        format = TRACING + "events/" + tp[0] + "/" + tp[1] + "/format"
        if not os.path.exists(format):
            return None

        fields = {}
        fields['field'] = {}
        with open(format) as file:
            # field:char comm[16];    offset:8;       size:16;        signed:1;
            prog  = re.compile(r'^field:[\S ]+ (\S+);\s+offset:(\d+);\s+size:(\d+);\s+signed:(\d+);$')
            prog_ = re.compile(r'^field:[\S ]+ (\S+)\[[\S ]+\];\s+offset:(\d+);\s+size:(\d+);\s+signed:(\d+);$')
            for line in file:
                s = line.strip()
                if s.startswith('name:'):
                    fields['name'] = s.split()[1]
                    continue
                if s.startswith('ID:'):
                    fields['id'] = int(s.split()[1])
                    continue
                if s.startswith('field:'):
                    if s.find("];") != -1:
                        m = prog_.match(s)
                    else:
                        m = prog.match(s)
                    if m != None:
                        m.groups()
                        fields["field"][m.group(1)] = m.groups()[1:]
                    continue
                if s.startswith('print fmt:'):
                    fields['print_fmt'] = s[len('print fmt: '):]
                    continue
        return fields

    @staticmethod
    def pmu_exists(pmu):
        pmu_path = "/sys/bus/event_source/devices/" + pmu
        return os.path.exists(pmu_path)

    @staticmethod
    def tsc_clocksource():
        """--tsc needs the tsc clocksource; VMs usually run on kvm-clock."""
        try:
            with open("/sys/devices/system/clocksource/clocksource0/current_clocksource") as f:
                return f.read().strip() == "tsc"
        except OSError:
            return False

    @staticmethod
    def standalone_python():
        """Return the path to standalone python3 binary, or None if not standalone."""
        perf_prof = PerfProf(['python', '-h'])
        found_pythonhome = False
        for std, line in perf_prof.run(_args_print=False):
            if found_pythonhome:
                path = line.strip()
                python3 = os.path.join(path, 'bin', 'python3')
                if os.path.isfile(python3):
                    return python3
                return None
            if 'PYTHONHOME' in line:
                found_pythonhome = True
        return None

    @staticmethod
    def btf_exists():
        return os.path.exists("/sys/kernel/btf/vmlinux")

class DeadLoop():
    def __init__(self, loop=lambda :True):
        self.tid = -1
        self._stop = False
        self._loop_ = loop
        self._t = threading.Thread(target=self._loop, name='Loop', daemon=True)
        self._t.start()
        while self.tid == -1:
            time.sleep(0.001)

    def _loop(self):
        self.tid = PerfProf.gettid()
        while not self._stop:
            self._loop_()

    def stop(self):
        self._stop = True
        self._t.join()


if __name__ == '__main__':
    print(PerfProf.kernel_release())
    print(PerfProf.gettid(), os.getpid())
    print(PerfProf.sysctl('kernel.sched_schedstats', '0'))
    print(PerfProf.profilers())
