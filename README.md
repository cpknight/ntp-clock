# :desktop_computer: Terminal NTP Clock

A terminal-based digital clock that synchronizes with Network Time Protocol (NTP) servers to display pretty accurate UTC time.

![NTP Clock Screenshot](screenshot-ntp-clock.png)

## Features

* Real-time clock display in terminal window
* NTP synchronization for accurate timekeeping
* Status bar with connection and synchronization information
* Low CPU usage design

## Requirements

* C compiler (gcc/clang recommended)
* POSIX-compliant system (Linux, macOS, BSD)
* ncurses library for terminal display
* NTP client libraries
* Internet connection for time synchronization

## Installation

1. Clone the repository:
   ```
   git clone https://github.com/cpknight/ntp-clock.git
   cd ntp-clock
   ```

2. Build the application:
   ```
   make
   ```
<!--
3. Install (optional):
   ```
   sudo make install
   ```
-->

## Usage

Run the clock with default settings:
```
./ntp-clock
```

<!--
Command line options:
```
./clock [options]

Options:
  -h, --help         Display this help message
  -12, --12hour      Use 12-hour time format (AM/PM)
  -24, --24hour      Use 24-hour time format
  -c, --color=NAME   Use specified color theme
  -s, --server=HOST  Specify NTP server to use
  -i, --interval=N   Set sync interval (in seconds)
  -b, --border       Display with decorative border
  -f, --fullscreen   Run in fullscreen mode
```
-->

Use `Ctrl-C` to quit the application while running.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

