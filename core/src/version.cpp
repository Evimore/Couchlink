#include "inputline/version.h"

#include <cctype>
#include <cstdint>
#include <vector>

namespace inputline {

  namespace {
    struct Parsed {
      std::vector<std::uint64_t> numbers;
      std::vector<std::string> prerelease;
      bool valid = false;
    };

    bool all_digits(const std::string &text) {
      if (text.empty()) {
        return false;
      }
      for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          return false;
        }
      }
      return true;
    }

    std::uint64_t to_number(const std::string &digits) {
      std::uint64_t value = 0;
      for (char c : digits) {
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        value = value > (UINT64_MAX - digit) / 10 ? UINT64_MAX : value * 10 + digit;
      }
      return value;
    }

    std::vector<std::string> split(const std::string &text, char separator) {
      std::vector<std::string> parts;
      std::string part;
      for (char c : text) {
        if (c == separator) {
          parts.push_back(part);
          part.clear();
        } else {
          part.push_back(c);
        }
      }
      parts.push_back(part);
      return parts;
    }

    Parsed parse(std::string text) {
      Parsed out;
      if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) {
        text.erase(0, 1);
      }
      const auto plus = text.find('+');
      if (plus != std::string::npos) {
        text.resize(plus);
      }
      std::string core = text;
      const auto dash = text.find('-');
      if (dash != std::string::npos) {
        core = text.substr(0, dash);
        out.prerelease = split(text.substr(dash + 1), '.');
        for (const auto &identifier : out.prerelease) {
          if (identifier.empty()) {
            return out;
          }
        }
      }
      for (const auto &part : split(core, '.')) {
        if (!all_digits(part)) {
          return out;
        }
        out.numbers.push_back(to_number(part));
      }
      out.valid = !out.numbers.empty();
      return out;
    }

    int compare_identifiers(const std::string &a, const std::string &b) {
      const bool a_number = all_digits(a);
      const bool b_number = all_digits(b);
      if (a_number && b_number) {
        const auto x = to_number(a);
        const auto y = to_number(b);
        return x < y ? -1 : x > y ? 1 : 0;
      }
      if (a_number != b_number) {
        return a_number ? -1 : 1;  // numeric identifiers sort first
      }
      return a < b ? -1 : a > b ? 1 : 0;
    }
  }  // namespace

  int compare_versions(const std::string &a, const std::string &b) {
    const Parsed x = parse(a);
    const Parsed y = parse(b);
    const std::size_t count = x.numbers.size() > y.numbers.size() ? x.numbers.size() : y.numbers.size();
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint64_t p = i < x.numbers.size() ? x.numbers[i] : 0;
      const std::uint64_t q = i < y.numbers.size() ? y.numbers[i] : 0;
      if (p != q) {
        return p < q ? -1 : 1;
      }
    }
    if (x.prerelease.empty() || y.prerelease.empty()) {
      return x.prerelease.empty() == y.prerelease.empty() ? 0 : x.prerelease.empty() ? 1 : -1;
    }
    for (std::size_t i = 0; i < x.prerelease.size() && i < y.prerelease.size(); ++i) {
      const int order = compare_identifiers(x.prerelease[i], y.prerelease[i]);
      if (order != 0) {
        return order;
      }
    }
    return x.prerelease.size() < y.prerelease.size() ? -1 : x.prerelease.size() > y.prerelease.size() ? 1 : 0;
  }

  bool is_version(const std::string &text) {
    return parse(text).valid;
  }

  bool is_prerelease(const std::string &version) {
    const Parsed parsed = parse(version);
    return parsed.valid && !parsed.prerelease.empty();
  }

}  // namespace inputline
