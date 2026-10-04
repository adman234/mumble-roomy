// Optional settings read from roomy.ini in Mumble's configuration directory.
#pragma once

#include "suppressor.h"

#include <string>

namespace roomy {

struct Config {
	bool enabled = true;
	/// How much the estimated bleed is overestimated before it is removed
	float strengthDb = 3.0f;
	/// The most any part of the signal is ever turned down
	float maxSuppressionDb = 30.0f;
	/// How long the microphone is held back to wait for the other microphone's audio
	int delayMs = 20;
	/// UDP port to use, 0 picks a free one
	int port = 0;

	SuppressorParams suppressorParams() const;
};

/// Where the settings file is expected: %APPDATA%\Mumble\roomy.ini on Windows,
/// $XDG_CONFIG_HOME/Mumble/roomy.ini (or ~/.config/Mumble/roomy.ini) elsewhere.
std::string configPath();

/// Reads "key = value" lines. Missing files, unknown keys and malformed values are ignored.
Config loadConfig(const std::string &path);

} // namespace roomy
