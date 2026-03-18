# perf-prof

**perf-prof** is a comprehensive Linux system-level analysis tool for long-term performance monitoring with low overhead, broad compatibility, and high reliability.

[![License](https://img.shields.io/badge/license-GPLv2-blue.svg)](LICENSE)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)](https://github.com/OpenCloudOS/perf-prof)

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Profilers](#profilers)
- [Documentation](#documentation)
- [Development](#development)
- [Contributing](#contributing)
- [License](#license)

## Overview

perf-prof is a user-space performance profiling tool built on top of `libperf`, `libtraceevent`, and `libbpf`. It provides real-time analysis without writing event data to disk, processing everything in memory and discarding it immediately after use.

### Key Features

- **In-Memory Processing**: Events are processed in memory and discarded immediately - no persistent storage overhead
- **Broad Compatibility**: Works with older Linux kernels (requires perf_event support)
- **User-Space Implementation**: Safe execution with rapid iteration capability
- **Modular Architecture**: 30+ specialized profilers for different analysis scenarios
- **Low Overhead**: Kernel-level filtering reduces data transfer to user space
- **Real-Time Analysis**: Process events as they occur with immediate feedback

## Installation

### Prerequisites

```bash
# Install required dependencies
yum install -y xz-devel elfutils-libelf-devel

# Optional: Install eBPF dependencies
yum install -y llvm bpftool
```

### Build from Source

```bash
# Clone the repository
git clone https://github.com/OpenCloudOS/perf-prof.git
cd perf-prof

# Build the project
make

# Verbose build
make V=1

# Clean build artifacts
make clean
```

### Cross-Compilation

```bash
# Using CROSS_COMPILE
make CROSS_COMPILE=aarch64-linux-gnu-

# Using LLVM
make LLVM=1
```

### Build with Standalone Python

To enable the Python profiler without system python3-devel, use [python-build-standalone](https://github.com/astral-sh/python-build-standalone):

```bash
# Download (auto-detects arch, default Python 3.12)
packages/download-python-standalone.sh

# Extract
tar xzf cpython-*.tar.gz

# Build with standalone Python
make PYTHON=python/bin/python3
```

The `PYTHON=` option automatically detects the standalone Python prefix, sets `PYTHONHOME`, and adds `-Wl,-rpath` for the bundled libpython. You can override `PYTHON_HOME=` to set a different install path.

## Quick Start

### List Available Profilers

```bash
# List all profilers
./perf-prof -h

# List all tracepoint events
./perf-prof list
```

### CPU Performance Analysis

```bash
# Profile CPU usage at 997Hz with call graph
./perf-prof profile -F 997 -g

# Use DWARF unwind for user-space callchain (more reliable than frame pointers)
./perf-prof profile -F 997 -g --user-callchain=dwarf

# Generate flame graph
./perf-prof profile -F 997 -g --flame-graph cpu.folded

# Profile user-space only on specific CPU
./perf-prof profile -F 997 -C 0-3 --exclude-kernel --than 30
```

### Memory Leak Detection

```bash
# Detect kernel memory leaks
./perf-prof kmemleak --alloc "kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/" \
                     --free "kmem:kfree//ptr=ptr/" --order -m 128 -g
```

### Process Scheduling Analysis

```bash
# Monitor task states (R, S, D, T, I)
./perf-prof task-state -i 1000
```

### Event Tracing

```bash
# Trace specific events
./perf-prof trace -e sched:sched_wakeup,sched:sched_switch -i 1000

# Trace with filtering
./perf-prof trace -e "sched:sched_wakeup/prio<10/" -i 1000
```

## Profilers

![perf-prof框架](docs/images/perf-prof_framework.png)

perf-prof provides 30+ specialized profilers organized by category:

### CPU Performance
- **profile** - CPU performance sampling analysis
- **oncpu** - Monitor processes running on CPU

### Memory Analysis
- **kmemleak** - Memory leak detection
- **kmemprof** - Memory allocation profiling
- **page-faults** - Page fault tracking

### Scheduling & Process
- **task-state** - Process state monitoring (R, S, D, T, I)
- **rundelay** - Scheduling run delay analysis

### I/O Performance
- **blktrace** - Block device I/O tracking

### Virtualization
- **kvm-exit** - KVM exit latency analysis
- **kvmmmu** - KVM MMU mapping observation

### Hardware Monitoring
- **hwstat** - Hardware state monitoring (cycles, IPC)
- **llcstat** - Last-level cache monitoring
- **tlbstat** - dTLB monitoring
- **ldlat-loads** - Intel load latency counting
- **ldlat-stores** - Intel store instruction counting
- **split-lock** - x86 split lock detection

### Interrupt & Timing
- **hrtimer** - High-resolution conditional sampling
- **irq-off** - Interrupt disabled detection
- **watchdog** - Hard/soft lock detection

### Data Analysis
- **sql** - SQL aggregation analysis with SQLite
- **top** - Key-value statistical analysis
- **multi-trace** - Multi-event relationship analysis
- **syscalls** - System call latency analysis
- **python** - Custom Python script event processing

### Utilities
- **trace** - Event tracking
- **list** - List tracepoint events
- **expr** - Expression-based test tool
- **usdt** - User Statically Defined Tracing
- **breakpoint** - Kernel/user space hardware breakpoints
- **kcore** - Read kernel memory
- **misc** - Miscellaneous tracking

## Event Selection

perf-prof follows a three-layer event selection specification:

### 1. Get System Events

```bash
# List all events
./perf-prof list

# Filter by category
./perf-prof list | grep -E "^(sched:|kmem:|timer:|irq:)"
```

### 2. View Event Help

```bash
# View event fields
./perf-prof trace -e sched:sched_wakeup help

# Multiple events
./perf-prof trace -e sched:sched_wakeup,sched:sched_switch help
```

### 3. Event Syntax

```
EVENT: sys:name[/filter/ATTR/ATTR/.../]
       kprobe:func[/filter/ATTR/ATTR/.../]
       kretprobe:func[/filter/ATTR/ATTR/.../]
       uprobe:func@"file"[/filter/ATTR/ATTR/.../]
       uretprobe:func@"file"[/filter/ATTR/ATTR/.../]
```

#### Filter Syntax (in-kernel execution)

```bash
# Numeric comparison
./perf-prof trace -e "sched:sched_wakeup/pid>1000/"
./perf-prof trace -e "sched:sched_wakeup/prio<10/"

# String matching
./perf-prof trace -e 'sched:sched_wakeup/comm=="java"/'
./perf-prof trace -e 'sched:sched_wakeup/comm~"pyth*"/'

# Logical combinations
./perf-prof trace -e "sched:sched_wakeup/pid>1000 && prio<10/"
```

#### Attributes (user-space execution)

```bash
stack                    # Enable call stack
alias=str                # Event alias
max-stack=int            # Max stack depth
key=EXPR                 # Event key for correlation
top-by=EXPR              # Sort field
comm=EXPR                # Process name display
ptr=EXPR                 # Pointer field
size=EXPR                # Size field
num=EXPR                 # Number distribution field
```

## Help System

```bash
# profiler help with examples
./perf-prof trace -h
./perf-prof task-state -h

# Event help with field information
./perf-prof trace -e sched:sched_wakeup help
./perf-prof kmemleak --alloc kmem:kmalloc --free kmem:kfree help
```

## Documentation

### Main Documentation

- [Main Options Reference](docs/main_options.md) - Complete command-line options

### Profiler Documentation

- [profile](docs/profilers/profile.md) - CPU performance analysis
- [task-state](docs/profilers/task-state.md) - Process state monitoring
- [multi-trace](docs/profilers/multi-trace.md) - Multi-event analysis
- [sql](docs/profilers/sql.md) - SQL aggregation analysis
- [top](docs/profilers/top.md) - Key-value statistics
- [kmemleak](docs/profilers/kmemleak.md) - Memory leak detection
- [kvm-exit](docs/profilers/kvm-exit.md) - KVM exit analysis
- [blktrace](docs/profilers/blktrace.md) - Block device I/O tracking
- [trace](docs/profilers/trace.md) - Event tracing
- [python](docs/profilers/python.md) - Python script event processing

### Advanced Topics

- [Event Filtering](docs/Event_filtering.md) - Trace event filter syntax
- [Expressions](docs/expr.md) - Expression language reference

### Translations

- [README - 中文版](README_CN.md) - Chinese README

## Testing

```bash
# Run all tests
cd tests
pytest

# Run specific test file
pytest test_profile.py

# Run with custom runtime and memory leak check
pytest --runtime=20 --memleak-check=2000
```

## Development

### Project Structure

```
perf-prof/
├── *.c                   # Core profiler modules (30+ profilers)
├── lib/                  # Base libraries (libperf, libtraceevent, libbpf)
├── arch/                 # Architecture-specific code
├── bpf-skel/             # BPF skeleton programs
├── filter/               # Event filters (BPF, tracepoint, PMU)
├── sqlite/               # SQLite amalgamation source code and extension modules
├── include/              # Included header files
├── tests/                # Test suite
├── docs/                 # Documentation
└── skills/               # AI-assisted analysis skill packages
```

### Core Components

**Monitoring Framework:**
- `monitor.c/h` - Core framework
- `tep.c/h` - Trace event parser
- `trace_helpers.c/h` - Trace event utilities
- `stack_helpers.c/h` - Stack traversal and symbol resolution

**Profiling Units:**
- Each profiler is an independent `.c` file
- Registered via `PROFILER_REGISTER()` macro
- Supports `init`, `deinit`, `interval`, `read`, `sample` callbacks

### Event Processing Pipeline

```
Event Source → Filters → Ring Buffer → Sort → Profiler → Output
```

### Adding a New Profiler

1. Create source file `new_profiler.c`
2. Implement `profiler` structure with required callbacks
3. Define `name`, `desc`, `argc`, `option`
4. Register with `PROFILER_REGISTER()`
5. Add to `Build` file: `perf-prof-y += new_profiler.o`
6. Add test in `tests/` directory

## Contributing

We welcome contributions! Please follow these guidelines:

1. Fork the repository
2. Create a feature branch
3. Write tests for new functionality
4. Ensure all tests pass
5. Submit a pull request

## License

This project is licensed under GPLv2. See [LICENSE](LICENSE) for details.

## Code of Conduct

- Follow Linux kernel coding style
- Write clear, maintainable code
- Include tests for new features
- Document public APIs and interfaces

## Links

- [GitHub Repository](https://github.com/OpenCloudOS/perf-prof)
- [Issue Tracker](https://github.com/OpenCloudOS/perf-prof/issues)
- [Documentation](docs/)
- [中文文档](README_CN.md)

## Acknowledgments

Built with components from the Linux kernel:
- libperf
- libtraceevent
- libbpf
- Additional utility libraries
