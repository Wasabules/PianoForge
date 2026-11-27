# SPDX-License-Identifier: AGPL-3.0-or-later

"""PianoForge profile builder: a sample bank (SFZ) to the modal profile the engine plays.

The chain, each step resumable: the bank fetched (or checked), described as a bench state (bench prepare-sfz), the
measurement sessions rendered (bench render: the passes the forge reads), the velocity layers and the pedal-down
samples found in the bank, the forge pipeline run (forge pipeline: analysis, modes, soundboard, dampers, noises,
pedal, sympathetic resonance, export), the profile checked. See docs/building-a-profile.md.
"""

from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
__version__ = "0.1.0"
