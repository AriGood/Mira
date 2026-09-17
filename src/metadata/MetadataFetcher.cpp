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

// Downloads `url` into this game's artwork dir under `slot` (fails closed
// on any non-2xx via curl -f, so a 404 never gets saved as if it were art),
// recording it into `info[slot == "cover" ? "artwork" : slot]` if it
// succeeds -- "artwork" rather than "cover" for the cover slot specifically
// is legacy naming kept for wire compatibility with what
// GET /v1/games/{id}/metadata already documented before "hero" existed.
// Extension is taken from the url itself, since that's the only place
// either source says what format it sent.
void FetchArtworkInto(const config::Config& config, const std::string& url, const std::string& game_id,
                      std::string_view source, std::string_view slot, json& info,
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
  const fs::path dest = dir / (std::string(slot) + ext);

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
  const std::string key = slot == "cover" ? "artwork" : std::string(slot);
  info[key] = {{"file", dest.filename().string()}, {"content_type", ContentTypeFor(dest)},
              {"source", source}};
}

void FetchSteamOwned(const config::Config& config, const std::string& appid, const std::string& game_id,
                     json& info) {
  const json store = CurlJson(
      {"curl", "-sSL", std::format("https://store.steampowered.com/api/appdetails?appids={}&l=english", appid)});
  if (!store.is_discarded() && store.contains(appid) && store[appid].value("success", false)) {
    const json& data = store[appid]["data"];
    json steam_info = {
        {"appid", appid},
        {"short_description", data.value("short_description", std::string())},
        {"release_date", data.value("release_date", json::object()).value("date", std::string())},
        {"is_free", data.value("is_free", false)},
        {"developers", data.value("developers", json::array())},
        {"publishers", data.value("publishers", json::array())},
        {"website", data.value("website", std::string())},
    };
    if (data.contains("metacritic")) steam_info["metacritic_score"] = data["metacritic"].value("score", 0);
    if (data.contains("price_overview")) {
      steam_info["price"] = data["price_overview"].value("final_formatted", std::string());
    }
    if (data.contains("recommendations")) {
      steam_info["recommendations_total"] = data["recommendations"].value("total", 0);
    }
    json genres = json::array();
    for (const auto& genre : data.value("genres", json::array())) {
      if (genre.contains("description")) genres.push_back(genre["description"]);
    }
    steam_info["genres"] = genres;
    json categories = json::array();
    for (const auto& category : data.value("categories", json::array())) {
      if (category.contains("description")) categories.push_back(category["description"]);
    }
    steam_info["categories"] = categories;

    if (data.contains("header_image")) steam_info["header_image_url"] = data["header_image"];
    if (data.contains("background_raw")) steam_info["background_url"] = data["background_raw"];
    steam_info["supported_languages"] = data.value("supported_languages", std::string());
    if (data.contains("pc_requirements") && data["pc_requirements"].is_object()) {
      steam_info["pc_requirements"] = {
          {"minimum", data["pc_requirements"].value("minimum", std::string())},
          {"recommended", data["pc_requirements"].value("recommended", std::string())},
      };
    }
    json dlc = json::array();
    for (const auto& id : data.value("dlc", json::array())) dlc.push_back(id);
    steam_info["dlc"] = dlc;
    json descriptors = json::array();
    if (data.contains("content_descriptors") && data["content_descriptors"].is_object()) {
      for (const auto& note : data["content_descriptors"].value("notes", json::array())) descriptors.push_back(note);
    }
    steam_info["content_descriptors"] = descriptors;
    if (data.contains("achievements")) {
      steam_info["achievements_total"] = data["achievements"].value("total", 0);
    }
    json screenshots = json::array();
    for (const auto& shot : data.value("screenshots", json::array())) {
      const std::string url = shot.value("path_full", std::string());
      if (!url.empty()) screenshots.push_back(url);
    }
    steam_info["screenshots"] = screenshots;
    json movies = json::array();
    for (const auto& movie : data.value("movies", json::array())) {
      if (!movie.contains("mp4")) continue;
      const std::string url = movie["mp4"].value("max", std::string());
      if (!url.empty()) movies.push_back(url);
    }
    steam_info["movies"] = movies;

    info["steam"] = steam_info;
  }

  // Not part of appdetails — Steam's user review summary is a separate
  // public endpoint (still no key needed), so it's a second call rather than
  // a field pulled off `store` above.
  const json reviews = CurlJson({"curl", "-sSL",
                                 std::format("https://store.steampowered.com/appreviews/{}?json=1&language=all"
                                            "&purchase_type=all&num_per_page=0",
                                            appid)});
  if (!reviews.is_discarded() && reviews.value("success", 0) == 1 && reviews.contains("query_summary")) {
    const json& summary = reviews["query_summary"];
    info["steam_reviews"] = {
        {"score_description", summary.value("review_score_desc", std::string())},
        {"total_positive", summary.value("total_positive", 0)},
        {"total_negative", summary.value("total_negative", 0)},
        {"total_reviews", summary.value("total_reviews", 0)},
    };
  }

  const json proton =
      CurlJson({"curl", "-sSL", std::format("https://www.protondb.com/api/v1/reports/summaries/{}.json", appid)});
  if (!proton.is_discarded() && proton.contains("tier")) {
    info["protondb"] = {{"tier", proton.value("tier", std::string())},
                        {"confidence", proton.value("confidence", std::string())},
                        {"total_reports", proton.value("total", 0)}};
  }

  FetchArtworkInto(config, std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/library_600x900.jpg", appid),
                   game_id, "steam_cdn", "cover", info);
  // Steam's own CDN serves this too, same appid, no key -- the wide banner
  // shown at the top of a game's store/library page, distinct from the
  // vertical library_600x900 cover above.
  FetchArtworkInto(config, std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/library_hero.jpg", appid),
                   game_id, "steam_cdn", "hero", info);
  // Small store-listing thumbnail and the classic top-of-page banner --
  // same CDN, same no-key pattern, just two more fixed filenames per appid.
  FetchArtworkInto(config, std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/capsule_231x87.jpg", appid),
                   game_id, "steam_cdn", "capsule", info);
  FetchArtworkInto(config, std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/header.jpg", appid), game_id,
                   "steam_cdn", "header", info);
}

void FetchNonSteam(const config::Config& config, const std::string& name, const std::string& game_id, json& info) {
  const std::string api_key = config.GetString("steamgriddb.api_key");
  if (api_key.empty()) return;

  const std::string auth_header = std::format("Authorization: Bearer {}", api_key);
  const json search = CurlJson({"curl", "-sSL", "-H", auth_header,
                                std::format("https://www.steamgriddb.com/api/v2/search/autocomplete/{}",
                                           UrlEncode(name))});
  if (search.is_discarded() || !search.value("success", false) || search.value("data", json::array()).empty()) {
    return;
  }
  const std::int64_t griddb_id = search["data"][0].value("id", std::int64_t{0});
  if (griddb_id == 0) return;

  const json grids = CurlJson({"curl", "-sSL", "-H", auth_header,
                               std::format("https://www.steamgriddb.com/api/v2/grids/game/{}", griddb_id)});
  if (!grids.is_discarded() && grids.value("success", false) && !grids.value("data", json::array()).empty()) {
    const std::string url = grids["data"][0].value("url", std::string());
    if (!url.empty()) FetchArtworkInto(config, url, game_id, "steamgriddb", "cover", info, {"-H", auth_header});
  }

  // Same griddb_id already resolved above -- a separate SteamGridDB
  // endpoint, not a field on the grids response.
  const json heroes = CurlJson({"curl", "-sSL", "-H", auth_header,
                                std::format("https://www.steamgriddb.com/api/v2/heroes/game/{}", griddb_id)});
  if (!heroes.is_discarded() && heroes.value("success", false) && !heroes.value("data", json::array()).empty()) {
    const std::string url = heroes["data"][0].value("url", std::string());
    if (!url.empty()) FetchArtworkInto(config, url, game_id, "steamgriddb", "hero", info, {"-H", auth_header});
  }

  // Transparent logo (overlaid on hero/background in a GUI) and small
  // square icon -- two more SteamGridDB endpoints, same griddb_id, same
  // independent-of-each-other treatment as grids/heroes above.
  const json logos = CurlJson({"curl", "-sSL", "-H", auth_header,
                               std::format("https://www.steamgriddb.com/api/v2/logos/game/{}", griddb_id)});
  if (!logos.is_discarded() && logos.value("success", false) && !logos.value("data", json::array()).empty()) {
    const std::string url = logos["data"][0].value("url", std::string());
    if (!url.empty()) FetchArtworkInto(config, url, game_id, "steamgriddb", "logo", info, {"-H", auth_header});
  }
  const json icons = CurlJson({"curl", "-sSL", "-H", auth_header,
                               std::format("https://www.steamgriddb.com/api/v2/icons/game/{}", griddb_id)});
  if (!icons.is_discarded() && icons.value("success", false) && !icons.value("data", json::array()).empty()) {
    const std::string url = icons["data"][0].value("url", std::string());
    if (!url.empty()) FetchArtworkInto(config, url, game_id, "steamgriddb", "icon", info, {"-H", auth_header});
  }
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
