#pragma once

#include <cstdint>
#include <string>

// Asks GitHub at launch whether a newer release than this build is out
// (api.github.com, .../releases/latest), on a thread of its own. The answer is
// kept in <user>/update-check.txt, so a known newer release shows at once,
// even offline, until this build is that version or newer.
namespace PortUpdateCheck {

constexpr const char* kRepo = "Odrannnn/MetroidPrimePort";

struct Release {
  std::string version; // "0.19.0", no leading v
  std::string url;     // the release page
};

// <0, 0, >0 like strcmp, over dot-separated numbers ("0.18" == "0.18.0"). A
// leading 'v' is skipped. Anything else that isn't a digit or dot makes the
// text invalid, which compares below every valid version.
int CompareVersions(const std::string& a, const std::string& b);
// tag_name and html_url out of a releases/latest reply; false when it isn't one.
bool ParseLatestRelease(const std::string& json, Release& out);

// Starts (enabled) or stops checking. `current` is this build's version (empty:
// unknown, never check), `stateFile` where the last answer is kept. Cheap to
// call again with the same arguments.
void Configure(bool enabled, const std::string& current, const std::string& stateFile);
// Checks now, whatever the time since the last check (while enabled).
void CheckNow();

enum EStatus { kStatus_Off, kStatus_Checking, kStatus_UpToDate, kStatus_Available, kStatus_Failed };
EStatus Status();
// The newer release while Status() is kStatus_Available.
Release Latest();
std::string LastError();
// Unix time of the last successful check, 0 when never.
int64_t LastChecked();

} // namespace PortUpdateCheck
