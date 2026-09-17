#include "metadata/MetadataFetcher.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

#include <json.hpp>

#include "core/Command.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::metadata {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

// Every network call gets a hard ceiling: this runs unattended off a scan,
// not a user-triggered download, so an unreachable or hanging endpoint must
// never pile up a stuck background thread.
constexpr std::string_view kMaxTime = "10";

// Reads one optional field, treating "absent" and "present but null" the
// same way.
//
// This is not defensive decoration. nlohmann's value() throws
// type_error.302 when the key exists holding a different type, and JSON
// null is a different type — so `data.value("website", std::string())`
// throws on every Steam store page that has no website. Neon White,
// HoloCure and Armored Core VI are all such pages, and all three threw out
// of Fetch and ended up with no metadata and no cover at all. Every field
// read from a source we do not control goes through here.
template <typename T>
T Value(const json& object, const char* key, T fallback) {
  if (!object.is_object()) return fallback;
  const auto entry = object.find(key);
  if (entry == object.end() || entry->is_null()) return fallback;
  try {
    return entry->get<T>();
  } catch (const json::exception&) {
    // A field of an unexpected type is the source's problem, not a reason
    // to lose the rest of the record.
    return fallback;
  }
}

std::string UrlEncode(std::string_view input) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size());
  for (unsigned char c : input) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// Runs curl and parses its stdout as JSON. Empty/malformed output (offline,
// rate-limited, endpoint down) comes back as a discarded json rather than an
// error — every call site treats "couldn't get this source" as "skip it",
// not "fail the whole fetch".
json CurlJson(std::vector<std::string> argv) {
  argv.insert(argv.begin() + 1, {"--max-time", std::string(kMaxTime)});
  Command command;
  command.argv = std::move(argv);
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) return json();
  return json::parse(result->output, nullptr, false);
}

std::string ContentTypeFor(const fs::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext == ".png") return "image/png";
  if (ext == ".webp") return "image/webp";
  return "image/jpeg";
}

// Downloads `url` into this game's artwork dir (fails closed on any non-2xx
// via curl -f, so a 404 never gets saved as if it were art), recording it
// into `info` under "artwork" if it succeeds. Extension is taken from the
// url itself, since that's the only place either source says what format it
// sent.
void FetchArtworkInto(const config::Config& config, const std::string& url, const std::string& game_id,
                      std::string_view source, json& info,
                      const std::vector<std::string>& extra_curl_args = {}) {
  std::string ext = fs::path(std::string(url)).extension().string();
  if (ext.empty() || ext.size() > 5) ext = ".jpg";

  const fs::path dir = ArtworkDir(config, game_id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    log::Warn("couldn't create artwork dir for {}: {}", game_id, ec.message());
    return;
  }
  const fs::path dest = dir / ("cover" + ext);

  std::vector<std::string> argv = {"curl", "-sSL", "-f", "--max-time", std::string(kMaxTime)};
  argv.insert(argv.end(), extra_curl_args.begin(), extra_curl_args.end());
  argv.insert(argv.end(), {"-o", dest.string(), url});

  Command command;
  command.argv = std::move(argv);
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    fs::remove(dest, ec);
    return;
  }
  info["artwork"] = {{"file", dest.filename().string()}, {"content_type", ContentTypeFor(dest)},
                     {"source", source}};
}

void FetchSteamOwned(const config::Config& config, const std::string& appid, const std::string& game_id,
                     json& info) {
  const json store = CurlJson(
      {"curl", "-sSL", std::format("https://store.steampowered.com/api/appdetails?appids={}&l=english", appid)});
  if (!store.is_discarded() && store.contains(appid) && Value(store[appid], "success", false)) {
    const json data = Value(store[appid], "data", json::object());
    json steam_info = {
        {"appid", appid},
        {"short_description", Value(data, "short_description", std::string())},
        {"release_date", Value(Value(data, "release_date", json::object()), "date", std::string())},
        {"is_free", Value(data, "is_free", false)},
        {"developers", Value(data, "developers", json::array())},
        {"publishers", Value(data, "publishers", json::array())},
        {"website", Value(data, "website", std::string())},
    };
    // Written only when the store actually gave a value, so a free game
    // reads as "no price" rather than as a price of "".
    if (const int score = Value(Value(data, "metacritic", json::object()), "score", 0); score > 0) {
      steam_info["metacritic_score"] = score;
    }
    if (const std::string price =
            Value(Value(data, "price_overview", json::object()), "final_formatted", std::string());
        !price.empty()) {
      steam_info["price"] = price;
    }
    if (const int total = Value(Value(data, "recommendations", json::object()), "total", 0);
        total > 0) {
      steam_info["recommendations_total"] = total;
    }
    json genres = json::array();
    for (const auto& genre : Value(data, "genres", json::array())) {
      const std::string description = Value(genre, "description", std::string());
      if (!description.empty()) genres.push_back(description);
    }
    steam_info["genres"] = genres;
    json categories = json::array();
    for (const auto& category : Value(data, "categories", json::array())) {
      const std::string description = Value(category, "description", std::string());
      if (!description.empty()) categories.push_back(description);
    }
    steam_info["categories"] = categories;
    info["steam"] = steam_info;
  }

  // Not part of appdetails — Steam's user review summary is a separate
  // public endpoint (still no key needed), so it's a second call rather than
  // a field pulled off `store` above.
  const json reviews = CurlJson({"curl", "-sSL",
                                 std::format("https://store.steampowered.com/appreviews/{}?json=1&language=all"
                                            "&purchase_type=all&num_per_page=0",
                                            appid)});
  if (!reviews.is_discarded() && Value(reviews, "success", 0) == 1 && reviews.contains("query_summary")) {
    const json summary = Value(reviews, "query_summary", json::object());
    info["steam_reviews"] = {
        {"score_description", Value(summary, "review_score_desc", std::string())},
        {"total_positive", Value(summary, "total_positive", 0)},
        {"total_negative", Value(summary, "total_negative", 0)},
        {"total_reviews", Value(summary, "total_reviews", 0)},
    };
  }

  const json proton =
      CurlJson({"curl", "-sSL", std::format("https://www.protondb.com/api/v1/reports/summaries/{}.json", appid)});
  if (!proton.is_discarded() && proton.contains("tier")) {
    info["protondb"] = {{"tier", Value(proton, "tier", std::string())},
                        {"confidence", Value(proton, "confidence", std::string())},
                        {"total_reports", Value(proton, "total", 0)}};
  }

  FetchArtworkInto(config, std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/library_600x900.jpg", appid),
                   game_id, "steam_cdn", info);
}

void FetchNonSteam(const config::Config& config, const std::string& name, const std::string& game_id, json& info) {
  const std::string api_key = config.GetString("steamgriddb.api_key");
  if (api_key.empty()) return;

  const std::string auth_header = std::format("Authorization: Bearer {}", api_key);
  const json search = CurlJson({"curl", "-sSL", "-H", auth_header,
                                std::format("https://www.steamgriddb.com/api/v2/search/autocomplete/{}",
                                           UrlEncode(name))});
  if (search.is_discarded() || !Value(search, "success", false) ||
      Value(search, "data", json::array()).empty()) {
    return;
  }
  const std::int64_t griddb_id = Value(search["data"][0], "id", std::int64_t{0});
  if (griddb_id == 0) return;

  const json grids = CurlJson({"curl", "-sSL", "-H", auth_header,
                               std::format("https://www.steamgriddb.com/api/v2/grids/game/{}", griddb_id)});
  if (grids.is_discarded() || !Value(grids, "success", false) ||
      Value(grids, "data", json::array()).empty()) {
    return;
  }
  const std::string url = Value(grids["data"][0], "url", std::string());
  if (url.empty()) return;
  FetchArtworkInto(config, url, game_id, "steamgriddb", info, {"-H", auth_header});
}

}  // namespace

std::filesystem::path MetadataFile(const config::Config& config, const std::string& game_id) {
  return config.File().parent_path() / "metadata" / (game_id + ".json");
}

std::filesystem::path ArtworkDir(const config::Config& config, const std::string& game_id) {
  return config.File().parent_path() / "artwork" / game_id;
}

Result<void> Fetch(const config::Config& config, const model::Game& game) {
  json info = {{"fetched_at", model::NowSeconds()}};

  if (game.runner_ref.starts_with("steam:")) {
    info["source"] = "steam";
    FetchSteamOwned(config, game.runner_ref.substr(std::string_view("steam:").size()), game.id, info);
  } else {
    info["source"] = "steamgriddb";
    FetchNonSteam(config, game.name, game.id, info);
  }

  const fs::path metadata_file = MetadataFile(config, game.id);
  std::error_code ec;
  fs::create_directories(metadata_file.parent_path(), ec);
  if (ec) return Err("metadata_dir_failed", ec.message());

  std::ofstream out(metadata_file, std::ios::trunc);
  if (!out) return Err("metadata_write_failed", "couldn't open " + metadata_file.string() + " for writing");
  out << info.dump(2);
  return {};
}

}  // namespace mira::metadata
