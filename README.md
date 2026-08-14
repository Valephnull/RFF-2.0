# RFF-EXP

## What is this fork?

This fork (RFF Exploration Edition) adds a whole bunch of new exploration features, mostly from other fractal explorers, to RFF, with the goal of making a better experience deep zooming

## New Features

Currently, I have added:

* Newton-Raphson Zooming menu - basically Kalles Fraktaler's Newton-Raphson menu
* Auto Exploration menu - basically Fractal eXtreme's menu but better, and with more features
* Guided Zoom - enabled by default; uses Merutilm's auto-aim center first, with the Imagina-inspired largest-nearby-feature search as a fallback. Rapid wheel input is previewed continuously and coalesced into stable renders.
* Crash Recovery Autosave - silently saves the current center, zoom depth, and iteration limit once per second when they change. An abnormal exit offers Recover or Start Fresh at the next launch; a clean exit removes the recovery files.

## Guided Zoom Behavior

* Merutilm's `MB2Locator` center is the primary target when it is visible; nearby periodic and Misiurewicz detection remains as a fallback.
* Fallback orbit detection follows the current render iteration setting without an additional hidden ceiling. Newton refinement has no fixed pass limit.
* Mouse positions are grouped into 4-pixel cells and fallback searches are limited to once every 75 ms; rapid zooming reuses the locked target between renders.
* Guided Zoom requires a completed Power-2 Mandelbrot reference and does not search while Newton-Raphson Zooming owns navigation.

## Installation

Run `installer_windows.bat` as Administrator on Windows, or run `./installer_linux_ubuntu.sh` on Ubuntu 24.04 or newer.

