// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

namespace pf::app {

/// Headless check of the reference player:
///   PianoForge --selftest <folder of one corpus pass> <report file>
/// Returns 0 when every check passes.
int runSelfTest(const juce::StringArray& arguments);

} // namespace pf::app
