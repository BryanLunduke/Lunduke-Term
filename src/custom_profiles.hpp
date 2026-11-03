// SPDX-License-Identifier: GPL-3.0-or-later
// M6 item 4: user (custom) profiles. Each one is a key file in the user config dir,
// $XDG_CONFIG_HOME/lunduke-terminal/profiles/NAME.ini (next to settings.ini), with every option
// of a Profile: colours, palettes, fonts, margins, the bezel and every effect value. The file
// name is the profile's name; its id is "custom:NAME". Built-in profiles are read-only (they
// are compiled in); "Copy to custom profile" starts a custom one from any profile.
#pragma once

#include "profiles.hpp"

#include <string>
#include <vector>

namespace lterm {

inline constexpr const char* kCustomProfilePrefix = "custom:";
inline constexpr int kMaxProfileNameLength = 64;

bool isCustomProfileId(const std::string& id);
std::string customProfileId(const std::string& name);
std::string customProfileName(const std::string& id);  // "" when not a custom id

// $XDG_CONFIG_HOME/lunduke-terminal/profiles (g_get_user_config_dir).
std::string customProfilesDir();

// A name for a new or renamed custom profile: not empty or blank, at most 64 bytes, no path
// characters ('/', '\\'), no control characters, not starting with '.' or ending in blanks,
// and not the name or id of a built-in or of another custom profile (case-insensitive).
// `renaming` is the id being renamed (it may keep its own name, e.g. change its case).
bool validateProfileName(const std::string& name, std::string* err, const std::string& renaming = "");

// Serialization. Missing keys keep the values of the built-in "plain" profile; out-of-range
// numbers are clamped. A file that is not a key file, lacks [profile], or has a bad colour is
// corrupt (false + err).
std::string profileToIni(const Profile& p);
bool profileFromIni(const std::string& data, const std::string& name, Profile& out, std::string* err);

// Every *.ini in `dir`, sorted by name. Corrupt or unreadable files are skipped (a warning
// each) and left untouched on disk; a missing directory is no profiles.
std::vector<Profile> loadCustomProfiles(const std::string& dir, std::vector<std::string>* warnings = nullptr);
// loadCustomProfiles(customProfilesDir()) into the registry (findProfile / customProfiles()).
void reloadCustomProfiles(std::vector<std::string>* warnings = nullptr);

// Disk + registry. The id must be a custom id; built-ins are refused. Writes are atomic (a
// temporary file renamed over the old one); the directory is created (0700) when missing.
bool saveCustomProfile(const std::string& dir, const Profile& p, std::string* err);
bool renameCustomProfile(const std::string& dir, const std::string& id, const std::string& newName, std::string* err);
bool deleteCustomProfile(const std::string& dir, const std::string& id, std::string* err);

}  // namespace lterm
