/**
 * @file client_store.h
 * @brief Paired clients, persisted to a small text file.
 *
 * File format, one client per line:
 *   client <id as 8 hex digits> <key as 64 hex digits> <name>
 * Lines starting with '#' are comments. The file holds secrets; it is
 * written with owner-only permissions where the platform supports it.
 */
#pragma once

#include "inputline/link_protocol.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace inputline {

  struct PairedClient {
    std::uint32_t client_id = 0;
    link::Key key {};
    std::string name;
  };

  class ClientStore {
  public:
    ClientStore() = default;
    explicit ClientStore(std::string path);

    /** Load from disk. A missing file is an empty store. */
    bool load();
    bool save() const;

    std::optional<PairedClient> find(std::uint32_t client_id) const;
    void upsert(const PairedClient &client);
    bool remove(std::uint32_t client_id);
    std::vector<PairedClient> list() const;

    const std::string &path() const {
      return path_;
    }

    /** Default location: %APPDATA%\InputLine\inputline-host.conf or ~/.config/inputline/inputline-host.conf. */
    static std::string default_path();

  private:
    std::string path_;
    mutable std::mutex mutex_;
    std::vector<PairedClient> clients_;
  };

}  // namespace inputline
