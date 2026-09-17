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

// Downloads `url` into this game's artwork dir under `slot` (fails closed
// on any non-2xx via curl -f, so a 404 never gets saved as if it were art),
// recording it into `info[slot == "cover" ? "artwork" : slot]` if it
// succeeds -- "artwork" rather than "cover" for the cover slot specifically
// is legacy naming kept for wire compatibility with what
// GET /v1/games/{id}/metadata already documented before "hero" existed.
// Extension is taken from the url itself, since that's the only place
// either source says what format it sent. Returns whether the download
// actually landed, so a caller re-picking a slot (SelectArtwork below) can
// tell a bad pick from a good one instead of silently keeping stale info.
//
// Deliberately sends no credentials. Both sources put their images on a
// plain public CDN, and SteamGridDB's rejects an Authorization header for
// its own API with a 401 — so passing the key along, which is the obvious
// thing to do when the search that produced the url needed it, is what
// stopped every non-Steam game from ever getting a cover.
bool FetchArtworkInto(const config::Config& config, const std::string& url, const std::string& game_id,
                      std::string_view source, std::string_view slot, json& info) {
  std::string ext = fs::path(std::string(url)).extension().string();
  if (ext.empty() || ext.size() > 5) ext = ".jpg";

  const fs::path dir = ArtworkDir(config, game_id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    log::Warn("couldn't create artwork dir for {}: {}", game_id, ec.message());
    return false;
  }
  const fs::path dest = dir / (std::string(slot) + ext);

  Command command;
  command.argv = {"curl", "-sSL",         "-f", "--max-time", std::string(kMaxTime),
                  "-o",   dest.string(), url};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    log::Warn("couldn't download artwork for {} from {}", game_id, url);
    fs::remove(dest, ec);
    // The directory was created for a file that never arrived; leaving it
    // behind makes an empty artwork/<id>/ look like a cache entry. Harmless
    // if another slot already landed a file here -- remove() only clears an
    // empty directory.
    fs::remove(dir, ec);
    return false;
  }
  const std::string key = slot == "cover" ? "artwork" : std::string(slot);
  info[key] = {{"file", dest.filename().string()}, {"content_type", ContentTypeFor(dest)},
              {"source", source}};
  return true;
}

// Fetches every SteamGridDB candidate for one art slot (grids/heroes/logos/
// icons -> cover/hero/logo/icon), stores the full list in
// info["art_candidates"][slot] so a caller can offer a choice, and downloads
// the first (SteamGridDB's own top-ranked result) as the slot's default --
// same behaviour as before this existed, just without a second round trip
// once the user wants to pick a different one via SelectArtwork.
void FetchGriddbSlot(const config::Config& config, const std::string& auth_header, std::int64_t griddb_id,
                     const std::string& game_id, std::string_view endpoint, std::string_view slot, json& info) {
  const json response = CurlJson({"curl", "-sSL", "-H", auth_header,
                                  std::format("https://www.steamgriddb.com/api/v2/{}/game/{}", endpoint, griddb_id)});
  if (response.is_discarded() || !Value(response, "success", false)) return;
  const json data = Value(response, "data", json::array());
  if (data.empty()) return;

  json candidates = json::array();
  for (const auto& item : data) {
    const std::string url = Value(item, "url", std::string());
    if (url.empty()) continue;
    candidates.push_back({
        {"id", Value(item, "id", std::int64_t{0})},
        {"url", url},
        {"thumb", Value(item, "thumb", std::string())},
        {"width", Value(item, "width", 0)},
        {"height", Value(item, "height", 0)},
        {"style", Value(item, "style", std::string())},
    });
  }
  if (candidates.empty()) return;
  info["art_candidates"][std::string(slot)] = candidates;

  // No credentials on the image download itself -- see FetchArtworkInto.
  const std::string best_url = Value(candidates[0], "url", std::string());
  FetchArtworkInto(config, best_url, game_id, "steamgriddb", slot, info);
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

Result<void> FetchNonSteam(const config::Config& config, const std::string& name,
                           const std::string& game_id, json& info) {
  const std::string api_key = config.GetString("steamgriddb.api_key");
  // An error rather than a silent skip. There is no other free cover-art
  // source for a non-Steam game, so with no key there is nothing this
  // function can ever do — and reporting success left the caller with a
  // cache entry, a game.metadata_ready event and no picture, which reads as
  // "Mira looked and there was nothing" rather than "Mira was never given
  // the one thing it needed".
  if (api_key.empty()) {
    return Err("no_steamgriddb_key",
               "non-Steam games need a SteamGridDB API key for cover art — set "
               "steamgriddb.api_key (it is free, from steamgriddb.com)");
  }

  const std::string auth_header = std::format("Authorization: Bearer {}", api_key);
  const json search = CurlJson({"curl", "-sSL", "-H", auth_header,
                                std::format("https://www.steamgriddb.com/api/v2/search/autocomplete/{}",
                                           UrlEncode(name))});
  if (search.is_discarded() || !Value(search, "success", false) ||
      Value(search, "data", json::array()).empty()) {
    return {};  // no match by name is an ordinary outcome, not a failure
  }
  const std::int64_t griddb_id = Value(search["data"][0], "id", std::int64_t{0});
  if (griddb_id == 0) return {};

  // Every candidate for every slot goes into info["art_candidates"][slot]
  // (see FetchGriddbSlot) so a caller can offer a choice instead of only
  // ever getting SteamGridDB's top pick.
  FetchGriddbSlot(config, auth_header, griddb_id, game_id, "grids", "cover", info);
  // Same griddb_id already resolved above -- a separate SteamGridDB
  // endpoint, not a field on the grids response.
  FetchGriddbSlot(config, auth_header, griddb_id, game_id, "heroes", "hero", info);
  // Transparent logo (overlaid on hero/background in a GUI) and small
  // square icon -- two more SteamGridDB endpoints, same griddb_id, same
  // independent-of-each-other treatment as grids/heroes above.
  FetchGriddbSlot(config, auth_header, griddb_id, game_id, "logos", "logo", info);
  FetchGriddbSlot(config, auth_header, griddb_id, game_id, "icons", "icon", info);
  return {};
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
    // Returned before anything is written: a failure here means nothing was
    // fetched, and a cache file would make the next attempt look answered.
    if (Result<void> fetched = FetchNonSteam(config, game.name, game.id, info); !fetched) {
      return std::unexpected(fetched.error());
    }
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

Result<void> SelectArtwork(const config::Config& config, const std::string& game_id, const std::string& slot,
                           std::int64_t candidate_id) {
  const fs::path metadata_file = MetadataFile(config, game_id);
  std::ifstream in(metadata_file);
  if (!in) return Err("metadata_not_found", "no metadata cached for this game yet");
  json info = json::parse(in, nullptr, false);
  in.close();
  if (info.is_discarded()) return Err("metadata_not_found", "cached metadata is corrupt");

  if (!info.contains("art_candidates") || !info["art_candidates"].contains(slot)) {
    return Err("no_candidates", "no candidate list cached for this slot");
  }
  std::string url;
  for (const auto& candidate : info["art_candidates"][slot]) {
    if (Value(candidate, "id", std::int64_t{-1}) == candidate_id) {
      url = Value(candidate, "url", std::string());
      break;
    }
  }
  if (url.empty()) return Err("candidate_not_found", "no such candidate id for this slot");

  // Looked up by id against the list this same code already fetched and
  // cached, rather than accepting a caller-supplied URL directly -- so the
  // daemon never ends up fetching an arbitrary URL on the API's behalf. No
  // credentials on the download itself -- see FetchArtworkInto.
  if (!FetchArtworkInto(config, url, game_id, "steamgriddb", slot, info)) {
    return Err("download_failed", "couldn't download the selected image");
  }

  std::ofstream out(metadata_file, std::ios::trunc);
  if (!out) return Err("metadata_write_failed", "couldn't open " + metadata_file.string() + " for writing");
  out << info.dump(2);
  return {};
}

}  // namespace mira::metadata
