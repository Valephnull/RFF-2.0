# RFF-EXP

## What is this fork?
This fork (RFF Exploration Edition) adds a whole bunch of new exploration features, mostly from other fractal explorers, to RFF, with the goal of making a better experience deep zooming.

## New Features
Currently, I have added:
- Newton-Raphson Zooming menu - basically Kalles Fraktaler's Newton-Raphson menu
- Auto Exploration menu - basically Fractal eXtreme's menu but better, and with more features

There is one complete RFF-EXP edition. Newton-Raphson Zooming is included in it; there is no separate Newton-only download.

Planned:
- Imagina's smooth zoom redirection feature its so satisfying
- More

See the original RFF Repo for details on the original project

## Installation

Download or clone this repository, then run the installer from the directory where you want RFF-EXP installed:

- **Windows:** right-click `installer_windows.bat` and choose **Run as administrator**. It installs/updates MSYS2 dependencies, builds this fork, and installs `bin`, `res`, and `shaders` beside the installer.
- **Ubuntu 24.04 or newer:** run `chmod +x installer_linux_ubuntu.sh && ./installer_linux_ubuntu.sh`. It installs the required packages, supplies a private CMake when Ubuntu's version is too old, builds this fork, and installs the same runtime directories beside the script.

Both installers preserve a working installation until the replacement build is complete, restore it if deployment fails, and use `version.config` to skip unnecessary rebuilds.
