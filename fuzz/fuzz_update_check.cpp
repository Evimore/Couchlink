// Fuzzes what the update check reads from the internet (GitHub's release
// list) and the status file the tray icon reads.

#include "status_file.h"
#include "update_check.h"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace inputline;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  const std::string text(reinterpret_cast<const char *>(data), size);
  const auto releases = update::parse_releases(text);
  (void) update::newer_release(releases, "0.1.0");
  (void) update::newer_release(releases, "0.1.0-beta.1");
  (void) parse_status(text);
  return 0;
}
