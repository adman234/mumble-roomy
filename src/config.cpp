#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace roomy {

namespace {

	std::string trim(const std::string &text) {
		const char *whitespace  = " \t\r\n";
		const std::size_t begin = text.find_first_not_of(whitespace);
		if (begin == std::string::npos) {
			return std::string();
		}
		const std::size_t end = text.find_last_not_of(whitespace);
		return text.substr(begin, end - begin + 1);
	}

	std::string environment(const char *name) {
		const char *value = std::getenv(name);
		return value ? std::string(value) : std::string();
	}

} // namespace

SuppressorParams Config::suppressorParams() const {
	SuppressorParams params;
	params.overSubtraction = std::pow(10.0f, std::clamp(strengthDb, 0.0f, 12.0f) / 10.0f);
	params.floorGain       = std::pow(10.0f, -std::clamp(maxSuppressionDb, 0.0f, 60.0f) / 20.0f);
	params.delayFrames     = std::clamp(static_cast< int >(std::lround(delayMs / kFrameMs)), 0, 10);
	return params;
}

std::string configPath() {
#ifdef _WIN32
	const std::string base = environment("APPDATA");
	return base.empty() ? std::string() : base + "\\Mumble\\roomy.ini";
#else
	std::string base = environment("XDG_CONFIG_HOME");
	if (base.empty()) {
		const std::string home = environment("HOME");
		if (home.empty()) {
			return std::string();
		}
		base = home + "/.config";
	}
	return base + "/Mumble/roomy.ini";
#endif
}

Config loadConfig(const std::string &path) {
	Config config;
	if (path.empty()) {
		return config;
	}

	std::ifstream file(path);
	std::string line;
	while (std::getline(file, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') {
			continue;
		}
		const std::size_t separator = line.find('=');
		if (separator == std::string::npos) {
			continue;
		}
		const std::string key   = trim(line.substr(0, separator));
		const std::string value = trim(line.substr(separator + 1));

		char *end           = nullptr;
		const double number = std::strtod(value.c_str(), &end);
		const bool isNumber = end != value.c_str();

		if (key == "enabled") {
			config.enabled = !(value == "0" || value == "false" || value == "no" || value == "off");
		} else if (!isNumber) {
			continue;
		} else if (key == "strength_db") {
			config.strengthDb = static_cast< float >(number);
		} else if (key == "max_suppression_db") {
			config.maxSuppressionDb = static_cast< float >(number);
		} else if (key == "delay_ms") {
			config.delayMs = static_cast< int >(number);
		} else if (key == "port") {
			config.port = std::clamp(static_cast< int >(number), 0, 65535);
		}
	}

	return config;
}

} // namespace roomy
