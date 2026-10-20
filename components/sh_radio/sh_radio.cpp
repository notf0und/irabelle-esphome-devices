#include "sh_radio.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <strings.h>

namespace esphome {
namespace sh_radio {

static const char *const TAG = "sh_radio";

// radio-browser.info. Every station query asks for `hidebroken=true` and
// `order=name` (alphabetical, the user-facing default); the authoritative row
// total comes from the category counts (countries/tags/languages `stationcount`)
// and, for the unfiltered "Popular" list, from /json/stats.
static const char *const RB_BASE = "https://all.api.radio-browser.info";
// Pickers are ordered by station count (biggest first). The country picker asks
// for every country radio-browser knows (~241 today, ~13 kB of JSON) so the user
// can reach all of them; tags/languages are in the thousands, so those stay
// capped at the 40 biggest.
static const char *const PICKER_QUERY = "?hidebroken=true&order=stationcount&reverse=true&limit=40";
// `hidebroken=true` makes `stationcount` mean exactly "rows a search with
// hidebroken=true returns": measured on the live API, Argentina is 1140 here and
// 1140 rows when paged with hidebroken, so the pager total is exact.
static const char *const COUNTRIES_QUERY = "?hidebroken=true&order=stationcount&reverse=true&limit=400";

static void copy_field(char *dst, size_t size, const char *src) {
  if (size == 0)
    return;
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

static std::string upper_ascii(const std::string &in) {
  std::string out = in;
  for (char &c : out)
    c = (char) toupper((unsigned char) c);
  return out;
}

/// Turn a directory name into a single-line label.
///
/// radio-browser carries raw source metadata for many stations, and a lot of the
/// Russian/Ukrainian ones start with a literal CR/LF (e.g. "\r\nГарик Сукачёв").
/// LVGL renders '\n' as a LINE BREAK, so such a name is two lines tall; inside
/// the list row it is taller than the label box, and LVGL's SCROLL long mode
/// then starts a VERTICAL offset animation — the name visibly bounces up and
/// down. Collapsing every whitespace run (CR, LF, TAB, space) into one space and
/// trimming the tail keeps the name on one line. Leading whitespace is kept as a
/// single space on purpose: the server sorted the list on the raw name, so
/// trimming the front would make the visible order disagree with `order=name`.
static PsramString clean_name(const std::string &in) {
  PsramString out;
  out.reserve(in.size());
  bool prev_space = false;
  for (char ch : in) {
    const unsigned char c = (unsigned char) ch;
    const bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f');
    if (ws) {
      if (!prev_space)
        out.push_back(' ');
      prev_space = true;
    } else {
      out.push_back(ch);
      prev_space = false;
    }
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

// ── lifecycle ───────────────────────────────────────────────────────────────

void ShRadio::setup() {
  this->pref_ = global_preferences->make_preference(sizeof(FavStore), FAV_KEY);
  this->load_favourites_();
  this->status_ = std::to_string(this->favourites_.size()) + " favourites";
  // Directory GETs run here, NOT on the main loop: a slow DNS/TLS handshake
  // used to block the loop past the task-watchdog window and reset the device
  // ("Reason: Task wdt") as soon as paging made requests frequent.
  if (xTaskCreate(fetch_task_, "sh_radio_fetch", 8192, this, 1, &this->fetch_task_handle_) != pdPASS) {
    this->fetch_task_handle_ = nullptr;
    ESP_LOGE(TAG, "Failed to create the directory fetch task");
  }
  ESP_LOGI(TAG, "Radio ready: %u favourite(s), page size %d", (unsigned) this->favourites_.size(), PAGE_SIZE);
}

void ShRadio::dump_config() {
  ESP_LOGCONFIG(TAG,
                "SH Radio:\n"
                "  Max results: %d\n"
                "  Page size: %d\n"
                "  Favourites: %u",
                this->max_results_, PAGE_SIZE, (unsigned) this->favourites_.size());
}

void ShRadio::loop() {
  // Parse a directory response that the worker task has finished reading.
  this->pump_async_fetch_();

  // Latch "the station is actually audible" so the wake-word handler can arm a
  // resume even when the interruption has already changed the player state.
  // The latch must also drop when the station is paused/stopped by anything
  // other than stop() (the hero play/pause button pauses, HA or the media
  // player entity can stop it), otherwise the next wake word "resumes" a radio
  // the user had switched off. While a resume is armed the player is expected
  // to go IDLE (HA drops the proxy for the announcement), so keep it then, and
  // ANNOUNCING says nothing about the radio either way.
  if (this->player_ != nullptr) {
    const auto st = this->player_->state;
    if (st == media_player::MEDIA_PLAYER_STATE_PLAYING) {
      this->radio_was_playing_ = true;
    } else if (st != media_player::MEDIA_PLAYER_STATE_ANNOUNCING && !this->resume_pending_) {
      this->radio_was_playing_ = false;
    }
  }

  // Remember when a "Loading ..." status appeared. list_loading() keeps the
  // spinner up for a bounded grace period even with nothing in flight (the
  // status is set before the 250 ms script debounce runs), so the spinner can
  // never outlive the reason for it.
  if (this->status_.rfind("Loading", 0) == 0) {
    if (this->loading_since_ms_ == 0)
      this->loading_since_ms_ = millis();
  } else {
    this->loading_since_ms_ = 0;
  }

  // Trailing-edge debounce: a burst of row taps collapses into one stream
  // switch. Firing here (not in the LVGL click callback) keeps the media
  // pipeline churn off the touch path.
  if (this->pending_at_ != 0 && (uint32_t) (millis() - this->pending_at_) >= PLAY_DEBOUNCE_MS) {
    const std::string uuid = this->pending_uuid_;
    this->pending_at_ = 0;
    this->pending_index_ = -1;
    this->pending_uuid_.clear();
    if (this->coalesced_ > 0) {
      ESP_LOGI(TAG, "Rapid-tap debounce: %u tap(s) coalesced, opening the last selection only",
               (unsigned) this->coalesced_);
      this->coalesced_ = 0;
    }
    // The window may have changed while the timer ran; resolve by uuid.
    const auto &v = this->view_();
    int local = -1;
    for (size_t i = 0; i < v.size(); i++) {
      if (v[i].uuid.size() == uuid.size() && std::strcmp(v[i].uuid.c_str(), uuid.c_str()) == 0) {
        local = (int) i;
        break;
      }
    }
    if (local < 0) {
      ESP_LOGW(TAG, "Debounced play dropped: station left the current list");
    } else {
      this->start_play_(this->abs_from_local_(local));
    }
  }

  // A request that never reports back must not wedge the list page forever.
  // The relaxed check covers "requested but the worker never picked it up"; the
  // hard ceiling ALSO covers a wedged worker (HTTP/TLS/DNS inside
  // fetch_blocking_()), which the old check skipped entirely - that was the
  // second half of the "page 5 spins forever" report.
  const uint32_t elapsed = (uint32_t) (millis() - this->fetch_started_);
  const bool stuck_unpicked = !this->fetch_complete_ && !this->fetch_task_busy_ && elapsed > FETCH_TIMEOUT_MS;
  const bool stuck_worker = !this->fetch_complete_ && elapsed > FETCH_HARD_TIMEOUT_MS;
  if (this->fetching_ && (stuck_unpicked || stuck_worker)) {
    this->fetching_ = false;
    this->fetch_task_busy_ = false;  // release the spinner; a late reply still parses
    this->fetch_error_ = true;
    this->status_ = "Directory timeout";
    ESP_LOGW(TAG, "Directory fetch timed out after %u ms (%s)", (unsigned) elapsed,
             stuck_worker ? "worker stalled" : "worker never started");
    this->updated_trigger_.trigger();
  }

  // Home Assistant hand-off status. This is NOT a failure watchdog: it only
  // reports, it never auto-skips and it never re-routes through a device-side
  // codec path (there is none any more).
  if (this->ha_pending_) {
    if (this->is_playing()) {
      this->ha_pending_ = false;
      this->status_ = "Playing";
      this->updated_trigger_.trigger();
    } else if ((uint32_t) (millis() - this->ha_at_) > HA_START_TIMEOUT_MS) {
      this->ha_pending_ = false;
      this->status_ = "Home Assistant did not start the stream";
      ESP_LOGW(TAG, "Home Assistant did not start '%s' within %u ms", this->now_playing_.c_str(),
               (unsigned) HA_START_TIMEOUT_MS);
      this->updated_trigger_.trigger();
    }
  }

  // Background preload (request 8): keep the previous and the next page resident
  // so a pager tap renders instantly. Single-flight (prefetch_needed() is false
  // while a request is in flight) and throttled/backed off so a dead network
  // cannot be hammered with TLS handshakes. The window is ARMED here (not merely
  // fired): a bare re-fetch would re-request the window already on screen and
  // never cover the page the UI moved to.
  if ((uint32_t) (millis() - this->last_prefetch_ms_) >= PREFETCH_INTERVAL_MS &&
      (int32_t) (millis() - this->prefetch_blocked_until_) >= 0 && this->prefetch_needed()) {
    if (this->arm_window_(this->page_)) {
      this->last_prefetch_ms_ = millis();
      this->prefetch_trigger_.trigger();
    }
  }
}

// ── directory ───────────────────────────────────────────────────────────────

std::string ShRadio::search_url() {
  this->fetching_ = true;
  this->fetch_started_ = millis();
  this->fetch_kind_ = this->kind_;
  this->fetch_stats_ = false;

  switch (this->kind_) {
    case ListKind::COUNTRIES:
      this->fetch_label_ = "countries";
      return std::string(RB_BASE) + "/json/countries" + COUNTRIES_QUERY;
    case ListKind::TAGS:
      this->fetch_label_ = "categories";
      return std::string(RB_BASE) + "/json/tags" + PICKER_QUERY;
    case ListKind::LANGUAGES:
      this->fetch_label_ = "languages";
      return std::string(RB_BASE) + "/json/languages" + PICKER_QUERY;
    default:
      break;
  }

  switch (this->mode_) {
    case BrowseMode::POPULAR:
      this->fetch_label_ = "popular";
      break;
    case BrowseMode::COUNTRY:
      this->fetch_label_ = "country " + this->sel_code_;
      break;
    case BrowseMode::TAG:
      this->fetch_label_ = "tag " + this->sel_code_;
      break;
    default:
      this->fetch_label_ = "language " + this->sel_code_;
      break;
  }

  // The unfiltered "Popular" list has no category count to read, so pull the
  // tiny /json/stats object first; its `stations` field is exactly the number of
  // rows this search returns with hidebroken=true (verified: 58319 == the last
  // non-empty offset). parse_json_buffer_ then immediately requests the list.
  if (this->mode_ == BrowseMode::POPULAR && !this->stats_loaded_) {
    this->fetch_label_ = "popular total";
    this->fetch_stats_ = true;
    return std::string(RB_BASE) + "/json/stats";
  }

  // Page-aligned window fetch. `window_request_` is armed by arm_window_() with
  // the page the UI moved to; otherwise this is a refresh of the window already
  // on screen.
  if (this->window_request_) {
    this->window_request_ = false;
  } else {
    this->window_start_page_ = this->win_page_;
  }
  this->fetch_offset_ = this->window_start_page_ * PAGE_SIZE;
  // Put the requested offset in the log label: the GET line is the only place a
  // FAILED window request is visible (a truncated/failed body never reaches the
  // "Window in ... at offset" line), so the offset must be in the GET line.
  this->fetch_label_ += " off " + std::to_string(this->fetch_offset_);

  // No `codec=` parameter: radio-browser accepts only a single codec value and a
  // comma list returns zero rows, and it is pointless now anyway - every station
  // is played through Home Assistant, which transcodes whatever the source uses.
  // `order=name` is the alphabetical default (verified against the live API).
  std::string url = std::string(RB_BASE) +
                    "/json/stations/search?hidebroken=true&order=name&limit=" +
                    std::to_string(this->max_results_) + "&offset=" + std::to_string(this->fetch_offset_);
  switch (this->mode_) {
    case BrowseMode::COUNTRY:
      if (!this->sel_code_.empty())
        url += "&countrycode=" + this->sel_code_;
      break;
    case BrowseMode::TAG:
      // tagExact makes the search return exactly the rows the /json/tags
      // `stationcount` advertises (measured: rock -> 2960 vs 2961, live DB),
      // so the pager total is the real one instead of ~1.75x too small.
      if (!this->sel_code_.empty())
        url += "&tagList=" + this->sel_code_ + "&tagExact=true";
      break;
    case BrowseMode::LANGUAGE:
      // Same for languages: languageExact matched the picker counts exactly
      // (spanish 5025 == 5025, english 11954 == 11954).
      if (!this->sel_code_.empty())
        url += "&language=" + this->sel_code_ + "&languageExact=true";
      break;
    default:
      break;
  }
  return url;
}

bool ShRadio::parse_json_buffer_(const uint8_t *data, size_t len) {
  // ESPHome's json component parses with a PSRAM-backed allocator, so the
  // document itself never competes with the internal heap.
  JsonDocument doc = json::parse_json(data, len);

  if (this->fetch_stats_) {
    this->fetch_stats_ = false;
    this->fetching_ = false;
    JsonObject o = doc.as<JsonObject>();
    const int total = o.isNull() ? 0 : o["stations"].as<int>();
    this->stats_loaded_ = total > 0;
    if (total > 0)
      this->total_rows_ = total;
    ESP_LOGI(TAG, "Popular total: %d station(s)", total);
    // The list itself is pulled next (reuses the pending-request mechanism).
    this->fetch_pending_ = true;
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    this->status_ = "Bad response";
    this->fetch_error_ = true;
    ESP_LOGW(TAG, "Response was not a JSON array (%u bytes) for %s", (unsigned) len, this->fetch_url_.c_str());
    this->fetching_ = false;
    this->updated_trigger_.trigger();
    return false;
  }
  this->fetch_error_ = false;
  return this->parse_array_(arr);
}

bool ShRadio::parse_array_(JsonArray arr) {
  const ListKind want = this->fetch_kind_;
  this->fetching_ = false;

  // Sniff the payload: the directory returns a different shape per endpoint.
  // (stations carry `stationuuid`, countries `iso_3166_1`, languages `iso_639`,
  // tags only a name + stationcount.)
  ListKind kind = want;
  JsonObject first = arr[0].as<JsonObject>();
  if (!first.isNull()) {
    if (!first["stationuuid"].isNull()) {
      kind = ListKind::STATIONS;
    } else if (!first["iso_3166_1"].isNull()) {
      kind = ListKind::COUNTRIES;
    } else if (!first["iso_639"].isNull()) {
      kind = ListKind::LANGUAGES;
    } else if (!first["name"].isNull() && !first["stationcount"].isNull()) {
      kind = ListKind::TAGS;
    }
  }

  StationList out;
  out.reserve(kind == ListKind::STATIONS ? (size_t) this->max_results_
                                         : (kind == ListKind::COUNTRIES ? 256u : 48u));
  const int raw = (int) arr.size();
  for (JsonObject o : arr) {
    if (kind == ListKind::STATIONS) {
      if ((int) out.size() >= this->max_results_)
        break;
      // No codec/HLS/broken filtering here any more: hidebroken=true is applied
      // server-side (so the server's row total is exactly what we page through)
      // and every codec is playable through Home Assistant's ffmpeg proxy.
      ShRadioStation s;
      s.uuid = o["stationuuid"].as<std::string>();
      s.name = clean_name(o["name"].as<std::string>());
      s.url = o["url_resolved"].as<std::string>();
      if (s.url.empty())
        s.url = o["url"].as<std::string>();
      copy_field(s.country, sizeof(s.country), upper_ascii(o["countrycode"].as<std::string>()).c_str());
      copy_field(s.country_name, sizeof(s.country_name), o["country"].as<std::string>().c_str());
      // Kept for the logs only: there is no codec decision left to make.
      copy_field(s.codec, sizeof(s.codec), o["codec"].as<std::string>().c_str());
      s.bitrate = (int16_t) o["bitrate"].as<int>();
      if (s.name.empty() || s.url.empty())
        continue;
      out.push_back(std::move(s));
    } else if (kind == ListKind::COUNTRIES) {
      ShRadioStation s;
      s.name = clean_name(o["name"].as<std::string>());
      s.url = upper_ascii(o["iso_3166_1"].as<std::string>());
      copy_field(s.country, sizeof(s.country), s.url.c_str());
      s.count = (int32_t) o["stationcount"].as<int>();
      if (s.name.empty() || s.url.empty())
        continue;  // without an ISO code there is nothing to query
      out.push_back(std::move(s));
    } else {  // TAGS / LANGUAGES: the query value is the name itself
      const std::string raw = o["name"].as<std::string>();
      ShRadioStation s;
      s.name = clean_name(raw);
      s.url = raw;  // the QUERY value stays exactly as the API reported it
      s.count = (int32_t) o["stationcount"].as<int>();
      if (s.name.empty())
        continue;
      out.push_back(std::move(s));
    }
  }

  if (kind == ListKind::COUNTRIES) {
    // The API returns the biggest countries first; alphabetical is easier to
    // scan once the list is on the screen.
    std::sort(out.begin(), out.end(), [](const ShRadioStation &a, const ShRadioStation &b) {
      return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
  }

  const int kept = (int) out.size();
  const char *noun = "stations";
  bool stations = false;
  switch (kind) {
    case ListKind::COUNTRIES:
      this->picker_ = std::move(out);
      this->picker_kind_ = ListKind::COUNTRIES;
      this->picker_valid_ = true;
      noun = "countries";
      break;
    case ListKind::TAGS:
      this->picker_ = std::move(out);
      this->picker_kind_ = ListKind::TAGS;
      this->picker_valid_ = true;
      noun = "categories";
      break;
    case ListKind::LANGUAGES:
      this->picker_ = std::move(out);
      this->picker_kind_ = ListKind::LANGUAGES;
      this->picker_valid_ = true;
      noun = "languages";
      break;
    default: {
      stations = true;
      // REPLACE the resident window with this server batch. `results_` starts at
      // page `win_page_`, so an absolute row index is win_page_*PAGE_SIZE+local.
      this->win_page_ = this->fetch_offset_ / PAGE_SIZE;
      this->results_ = std::move(out);
      if ((int) this->results_.size() > MAX_RESIDENT)
        this->results_.resize(MAX_RESIDENT);  // unreachable safety net
      const int window_end = this->fetch_offset_ + (int) this->results_.size();
      // A short batch means the server has no more rows here. The category
      // counts are live and can drift by a row or two (measured: tag rock 2961
      // vs 2960 rows), so trim the total to what actually exists.
      if (raw < this->max_results_ && this->total_rows_ > window_end)
        this->total_rows_ = window_end;
      ESP_LOGI(TAG, "Window in %d station(s) at offset %d (page %d of %d, total %d)", kept, this->fetch_offset_,
               this->win_page_ + 1, this->page_count(), this->total_rows_);
      break;
    }
  }

  if (this->kind_ == kind) {
    if (!stations)
      this->page_ = 0;
    int shown;
    if (stations)
      shown = this->total_rows_ > 0 ? this->total_rows_
                                    : (this->win_page_ * PAGE_SIZE + (int) this->results_.size());
    else
      shown = kept;
    this->status_ = std::to_string(shown) + " " + noun;
  }

  ESP_LOGI(TAG, "Parsed %d %s for '%s' (%d raw records)", kept, noun, this->fetch_label_.c_str(), raw);
  this->updated_trigger_.trigger();
  return true;
}

bool ShRadio::fetch_now() {
  // Non-blocking: build the URL on the main task (it mutates the paging cursor),
  // then hand the blocking GET to the worker task. The response is parsed from
  // loop(), on the main task, so LVGL/automation triggers stay safe.
  if (this->fetching_) {
    // The user picked another tab/country while a request was in flight. Do not
    // drop it: remember that a fresh fetch is wanted and start it (from the
    // then-current view) once the current response lands.
    this->fetch_pending_ = true;
    return true;
  }
  return this->start_fetch_();
}

bool ShRadio::start_fetch_() {
  this->fetch_pending_ = false;
  this->fetch_error_ = false;
  this->loading_since_ms_ = 0;
  this->fetch_url_ = this->search_url();  // sets fetching_/fetch_kind_/fetch_label_
  this->fetch_ok_ = false;
  this->fetch_why_.clear();
  this->fetch_complete_ = false;
  if (this->fetch_task_handle_ == nullptr) {
    this->fetching_ = false;
    this->fetch_error_ = true;
    this->status_ = "Directory worker unavailable";
    return false;
  }
  this->fetch_requested_ = true;
  xTaskNotifyGive(this->fetch_task_handle_);
  return true;
}

void ShRadio::fetch_task_(void *arg) {
  auto *self = static_cast<ShRadio *>(arg);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (self->fetch_requested_) {
      self->fetch_requested_ = false;
      self->fetch_task_busy_ = true;
      // One retry: a fresh TLS handshake occasionally fails (HTTP -1) on this
      // board while a stream is playing and the heap is at its tightest.
      bool ok = self->fetch_blocking_();
      if (!ok) {
        ESP_LOGW(TAG, "Directory fetch failed, retrying once");
        vTaskDelay(pdMS_TO_TICKS(1500));
        ok = self->fetch_blocking_();
      }
      self->fetch_ok_ = ok;
      self->fetch_task_busy_ = false;
      self->fetch_complete_ = true;
      App.wake_loop_threadsafe();  // parse on the main task
    }
  }
}

bool ShRadio::fetch_blocking_() {
  // Test hook (see test_fail_fetches): fail exactly like an unreachable host, so
  // the whole failure path (retry, status, error banner, spinner clearing) runs
  // for real. Never armed in normal operation.
  if (this->test_fail_left_ > 0) {
    this->test_fail_left_--;
    this->fetch_why_ = "Injected failure (test)";
    ESP_LOGW(TAG, "Test hook: failing this directory GET on purpose (%d left)", this->test_fail_left_);
    return false;
  }
  // The body buffer lives in PSRAM (megabytes free) instead of the internal DRAM
  // heap, which is down to a few tens of kB on this device once WiFi, the wake
  // word front-end, LVGL and the audio pipeline are up. FETCH_BUF_BYTES must
  // hold a WHOLE window: radio-browser returns complete station records and
  // Cyrillic names are multi-byte, so a 20-row RU window measured up to
  // 45 869 bytes. The old 32 768-byte cap silently truncated those bodies.
  const size_t cap = FETCH_BUF_BYTES;
  char *buf = (char *) heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (buf == nullptr) {
    this->fetch_why_ = "No PSRAM for the response";
    return false;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = this->fetch_url_.c_str();
  cfg.timeout_ms = HTTP_TIMEOUT_MS;
  cfg.disable_auto_redirect = false;
  cfg.max_redirection_count = 5;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.user_agent = "waveshare-amoled-216/1.0 (ESPHome sh_radio)";
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    heap_caps_free(buf);
    this->fetch_why_ = "HTTP client init failed";
    return false;
  }

  // NOTE: no WatchdogManager here on purpose. It reconfigures the *global* task
  // WDT; this runs on a worker task while the main loop keeps feeding the WDT,
  // so a slow DNS/TLS handshake can no longer reset the device.
  int status = 0;
  size_t total = 0;
  bool truncated = false;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    int read = 0;
    while (total + 1 < cap && (read = esp_http_client_read(client, buf + total, (int) (cap - 1 - total))) > 0) {
      total += read;
    }
    // The loop also stops when the buffer is full, which is NOT the same as the
    // body ending exactly at the cap. Probe one more byte to tell them apart:
    // a truncated body is invalid JSON, and its failure must be reported as
    // "too large", not as a generic parse error.
    if (total + 1 >= cap) {
      char extra = 0;
      if (esp_http_client_read(client, &extra, 1) > 0)
        truncated = true;
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  buf[total] = '\0';

  ESP_LOGI(TAG, "GET %s -> HTTP %d, %u bytes%s, esp_err %d (internal free %u, largest block %u, PSRAM free %u)",
           this->fetch_label_.c_str(), status, (unsigned) total, truncated ? " TRUNCATED" : "", (int) err,
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (err != ESP_OK || status != 200 || total == 0 || truncated) {
    heap_caps_free(buf);
    if (truncated) {
      this->fetch_why_ = "Response too large";
    } else {
      this->fetch_why_ = (status == 0) ? std::string("Directory unreachable") : ("HTTP " + std::to_string(status));
    }
    return false;
  }

  this->fetch_buf_ = buf;
  this->fetch_len_ = total;
  return true;
}

void ShRadio::pump_async_fetch_() {
  if (!this->fetch_complete_)
    return;
  this->fetch_complete_ = false;
  if (this->fetch_ok_ && this->fetch_buf_ != nullptr) {
    this->parse_json_buffer_((const uint8_t *) this->fetch_buf_, this->fetch_len_);
    this->prefetch_blocked_until_ = 0;
  } else {
    // Same bookkeeping the old fetch_failed() did, but on the main task (the
    // worker must not trigger automations).
    this->fetching_ = false;
    this->fetch_error_ = true;
    this->status_ = this->fetch_why_.empty() ? std::string("Directory unreachable") : this->fetch_why_;
    ESP_LOGW(TAG, "Directory fetch failed for %s: %s (%u bytes read)", this->fetch_url_.c_str(), this->status_.c_str(),
             (unsigned) this->fetch_len_);
    // Do not let the background preloader hammer a failing network.
    this->prefetch_blocked_until_ = millis() + FETCH_BACKOFF_MS;
    if (this->prefetch_blocked_until_ == 0)
      this->prefetch_blocked_until_ = 1;
    this->updated_trigger_.trigger();
  }
  if (this->fetch_buf_ != nullptr) {
    heap_caps_free(this->fetch_buf_);
    this->fetch_buf_ = nullptr;
  }
  this->fetch_len_ = 0;

  // A page change asked for a station that was not resident yet (hero next/prev
  // across the whole list): play it as soon as its window lands.
  if (this->pending_abs_play_ >= 0 && this->at_(this->pending_abs_play_) != nullptr) {
    const int abs = this->pending_abs_play_;
    this->pending_abs_play_ = -1;
    this->play(abs);
  }

  // A tab/country change arrived while this request was in flight: fetch it now
  // (search_url() re-reads the *current* view).
  if (this->fetch_pending_) {
    this->fetch_pending_ = false;
    this->start_fetch_();
  }
}

bool ShRadio::needs_fetch() const {
  if (this->kind_ == ListKind::FAVOURITES)
    return false;
  // Deliberately does NOT bail when a request is in flight: if the view now on
  // screen has no data (a tab/country just switched), fetch_now() queues a
  // follow-up instead of silently dropping the selection.
  return this->view_().empty();
}

// ── views ───────────────────────────────────────────────────────────────────

const ShRadio::StationList &ShRadio::view_() const {
  switch (this->kind_) {
    case ListKind::FAVOURITES:
      return this->favourites_;
    case ListKind::COUNTRIES:
    case ListKind::TAGS:
    case ListKind::LANGUAGES:
      // Only expose the picker when it belongs to the tab on screen: if a fetch
      // failed after a tab switch, showing the previous tab's list would be a
      // silent lie ("Per country" title with language rows).
      if (!this->picker_valid_ || this->picker_kind_ != this->kind_)
        return this->empty_;
      return this->picker_;
    case ListKind::STATIONS:
    default:
      return this->results_;
  }
}

const ShRadioStation *ShRadio::at_(int abs) const {
  if (abs < 0)
    return nullptr;
  if (this->kind_ == ListKind::STATIONS) {
    if (this->total_rows_ > 0 && abs >= this->total_rows_)
      return nullptr;
    const int local = abs - this->win_page_ * PAGE_SIZE;
    if (local < 0 || local >= (int) this->results_.size())
      return nullptr;
    return &this->results_[local];
  }
  if (abs >= (int) this->view_().size())
    return nullptr;
  return &this->view_()[abs];
}

int ShRadio::abs_from_local_(int local) const {
  if (local < 0)
    return -1;
  if (this->kind_ == ListKind::STATIONS)
    return this->win_page_ * PAGE_SIZE + local;
  return local;
}

bool ShRadio::picker_loaded_(ListKind k) const { return this->picker_valid_ && this->picker_kind_ == k; }

int ShRadio::picker_count_for_(const std::string &key) const {
  if (!this->picker_valid_ || key.empty())
    return 0;
  for (const auto &s : this->picker_) {
    if (this->picker_kind_ == ListKind::COUNTRIES) {
      if (strcasecmp(s.url.c_str(), key.c_str()) == 0)
        return (int) s.count;
    } else if (strcasecmp(s.name.c_str(), key.c_str()) == 0) {
      return (int) s.count;
    }
  }
  return 0;
}

std::string ShRadio::sub_for_(const ShRadioStation &s) {
  if (!s.uuid.empty()) {  // a station
    std::string r;
    if (s.country_name[0] != '\0')
      r = s.country_name;
    else if (s.country[0] != '\0')
      r = s.country;
    if (s.bitrate > 0) {
      if (!r.empty())
        r += " | ";
      r += std::to_string(s.bitrate) + "k";
    }
    return r.empty() ? "Favourite" : r;
  }
  if (s.count > 0)
    return std::to_string((long) s.count) + " stations";
  return "";
}

// ── paging ──────────────────────────────────────────────────────────────────

int ShRadio::page_count() const {
  if (this->kind_ == ListKind::STATIONS) {
    // The authoritative total is known from the first render (category count /
    // stats), so the real last page is known immediately.
    int n = this->total_rows_;
    if (n <= 0)
      n = this->win_page_ * PAGE_SIZE + (int) this->results_.size();
    return n <= 0 ? 1 : (n + PAGE_SIZE - 1) / PAGE_SIZE;
  }
  const int n = (int) this->view_().size();
  return n == 0 ? 1 : (n + PAGE_SIZE - 1) / PAGE_SIZE;
}

std::string ShRadio::page_label() const {
  return std::to_string(this->page_ + 1) + "/" + std::to_string(this->page_count());
}

void ShRadio::clamp_page_() {
  const int pc = this->page_count();
  if (this->page_ < 0)
    this->page_ = 0;
  if (this->page_ >= pc)
    this->page_ = pc - 1;
}

int ShRadio::window_pages_() const {
  const int wp = this->max_results_ / PAGE_SIZE;
  return wp < 1 ? 1 : wp;
}

bool ShRadio::window_covers_(int page) const {
  if (this->kind_ != ListKind::STATIONS)
    return true;
  if (this->results_.empty())
    return false;
  const int first = this->win_page_;
  const int last = this->win_page_ + ((int) this->results_.size() + PAGE_SIZE - 1) / PAGE_SIZE - 1;
  return page >= first && page <= last;
}

bool ShRadio::arm_window_(int page) {
  if (this->kind_ != ListKind::STATIONS)
    return false;
  const int pages = this->page_count();
  const int wp = this->window_pages_();
  int start = page - 1;  // one page of back-preload ...
  if (start < 0)
    start = 0;
  const int last_start = pages > wp ? pages - wp : 0;
  if (start > last_start)
    start = last_start;  // ... while still ending on the real last page
  this->window_start_page_ = start;
  // The window request must SURVIVE an in-flight fetch: the pending request
  // started when the current one lands re-reads window_request_/start, so it
  // must already point at the window the UI actually needs. (Forgetting this
  // made a wrap to the last page silently re-fetch offset 0 forever.)
  this->window_request_ = true;
  if (this->fetching_) {
    this->fetch_pending_ = true;  // re-request this window when the current lands
    return false;
  }
  // The page on screen has no rows until this lands: say so (the list page's
  // spinner is driven from list_loading()/status).
  if (this->row_index(0) < 0 && this->status_.rfind("Loading", 0) != 0) {
    this->status_ = "Loading...";
    this->updated_trigger_.trigger();
  }
  return true;
}

bool ShRadio::list_loading() const {
  // Favourites live in NVS: always instantly available.
  if (this->kind_ == ListKind::FAVOURITES)
    return false;
  // Rows for the visible page are resident -> nothing to wait for.
  if (this->row_index(0) >= 0)
    return false;
  // A request is in flight (or finished but not parsed yet): the rows are
  // genuinely on their way.
  if (this->fetching_ || this->fetch_complete_)
    return true;
  // "Loading ..." was announced (arm_window_/start_station_view_ set it before
  // the 250 ms script debounce runs, or before the fetch worker is handed the
  // request). Keep the spinner for a bounded grace period. `loading_since_ms_`
  // is 0 until loop() has recorded the status, and radio_refresh runs *before*
  // that on the same update - so a status that starts with "Loading" must count
  // as loading even with no timestamp yet (otherwise the spinner would never
  // appear at all, which is exactly what happened when this was a pure
  // timestamp check).
  if (this->status_.rfind("Loading", 0) == 0)
    return this->loading_since_ms_ == 0 ||
           (uint32_t) (millis() - this->loading_since_ms_) < LOADING_GRACE_MS;
  return false;
}

bool ShRadio::prefetch_needed() const {
  if (this->kind_ != ListKind::STATIONS)
    return false;
  if (this->fetching_)
    return false;  // single-flight
  if (this->results_.empty())
    return true;  // nothing resident yet
  const int pages = this->page_count();
  if (!this->window_covers_(this->page_))
    return true;
  if (this->page_ > 0 && !this->window_covers_(this->page_ - 1))
    return true;
  if (this->page_ + 1 < pages && !this->window_covers_(this->page_ + 1))
    return true;
  return false;
}

bool ShRadio::next_page() {
  const int pages = this->page_count();
  if (this->page_ + 1 >= pages)
    this->page_ = 0;  // wrap: last page -> the FIRST page (mirrors prev_page)
  else
    this->page_++;
  this->updated_trigger_.trigger();
  if (this->kind_ != ListKind::STATIONS)
    return false;
  if (!this->window_covers_(this->page_) || this->prefetch_needed())
    return this->arm_window_(this->page_);
  return false;
}

bool ShRadio::prev_page() {
  const int pages = this->page_count();
  if (this->page_ > 0)
    this->page_--;
  else
    this->page_ = pages - 1;  // wrap: page 1 -> the REAL last page
  this->updated_trigger_.trigger();
  if (this->kind_ != ListKind::STATIONS)
    return false;
  if (!this->window_covers_(this->page_) || this->prefetch_needed())
    return this->arm_window_(this->page_);
  return false;
}

int ShRadio::row_index(int row) const {
  const int i = this->page_ * PAGE_SIZE + row;
  if (i < 0)
    return -1;
  if (this->kind_ == ListKind::STATIONS) {
    if (this->total_rows_ > 0 && i >= this->total_rows_)
      return -1;
    const int local = i - this->win_page_ * PAGE_SIZE;
    if (local < 0 || local >= (int) this->results_.size())
      return -1;  // page selected but its rows are not in PSRAM yet
    return i;
  }
  if (i >= (int) this->view_().size())
    return -1;
  return i;
}

std::string ShRadio::row_name(int row) const {
  const auto *s = this->at_(this->row_index(row));
  return s == nullptr ? std::string("") : std::string(s->name.c_str());
}

std::string ShRadio::row_sub(int row) const {
  const auto *s = this->at_(this->row_index(row));
  return s == nullptr ? std::string("") : sub_for_(*s);
}

std::string ShRadio::row_flag(int row) const {
  const auto *s = this->at_(this->row_index(row));
  if (s == nullptr)
    return "";
  // Country flags: LVGL has no shaping/ligatures, so an emoji flag (a
  // regional-indicator pair) cannot be rendered as one glyph here. The ISO
  // code is shown instead.
  if (this->kind_ == ListKind::COUNTRIES || this->kind_ == ListKind::STATIONS)
    return std::string(s->country);
  return "";
}

std::string ShRadio::row_star(int row) const {
  const auto *s = this->at_(this->row_index(row));
  if (s == nullptr)
    return "\U000F04D2";
  return this->find_fav_(s->uuid.c_str()) >= 0 ? "\U000F04CE" : "\U000F04D2";
}

bool ShRadio::row_star_visible() const {
  return this->kind_ == ListKind::STATIONS || this->kind_ == ListKind::FAVOURITES;
}

// ── list navigation ─────────────────────────────────────────────────────────

int ShRadio::active_tab() const {
  switch (this->kind_) {
    case ListKind::FAVOURITES:
      return 4;
    case ListKind::COUNTRIES:
      return 0;
    case ListKind::TAGS:
      return 2;
    case ListKind::LANGUAGES:
      return 3;
    case ListKind::STATIONS:
    default:
      switch (this->mode_) {
        case BrowseMode::POPULAR:
          return 1;
        case BrowseMode::TAG:
          return 2;
        case BrowseMode::LANGUAGE:
          return 3;
        default:
          return 0;
      }
  }
}

std::string ShRadio::list_title() const {
  switch (this->kind_) {
    case ListKind::FAVOURITES:
      return "Favourites";
    case ListKind::COUNTRIES:
      return "Per country";
    case ListKind::TAGS:
      return "By category";
    case ListKind::LANGUAGES:
      return "By language";
    default:
      break;
  }
  if (this->mode_ == BrowseMode::POPULAR)
    return "Popular";
  return this->sel_label_.empty() ? "Stations" : this->sel_label_;
}

void ShRadio::show_picker_(ListKind k, const char *what) {
  this->kind_ = k;
  this->page_ = 0;
  this->total_rows_ = 0;
  this->win_page_ = 0;
  this->window_request_ = false;
  this->fetch_stats_ = false;
  this->results_.clear();  // free the station window; pickers do not use it
  if (this->picker_loaded_(k)) {
    this->status_ = std::to_string(this->view_().size()) + " " + what;
  } else {
    this->status_ = std::string("Loading ") + what + "...";
  }
  this->updated_trigger_.trigger();
}

void ShRadio::start_station_view_(BrowseMode m, const std::string &code, const std::string &label, int total) {
  this->mode_ = m;
  this->sel_code_ = code;
  this->sel_label_ = label.empty() ? code : label;
  this->kind_ = ListKind::STATIONS;
  this->page_ = 0;
  this->win_page_ = 0;
  this->results_.clear();
  this->window_request_ = false;
  this->fetch_stats_ = false;
  this->pending_abs_play_ = -1;
  if (m == BrowseMode::POPULAR) {
    // The exact total comes from /json/stats, fetched before the list.
    this->total_rows_ = 0;
    this->stats_loaded_ = false;
    this->status_ = "Loading popular...";
  } else {
    // Authoritative count straight from the category picker.
    this->total_rows_ = total > 0 ? total : 0;
    this->stats_loaded_ = true;
    this->window_request_ = true;
    this->window_start_page_ = 0;
    this->status_ = "Loading " + this->sel_label_ + "...";
  }
  ESP_LOGI(TAG, "Station view '%s' (%s) total=%d", this->sel_label_.c_str(), code.c_str(), this->total_rows_);
  this->updated_trigger_.trigger();
}

bool ShRadio::tab_select(int tab) {
  if (tab == 4) {
    this->kind_ = ListKind::FAVOURITES;
    this->page_ = 0;
    this->status_ = std::to_string(this->favourites_.size()) + " favourites";
    this->updated_trigger_.trigger();
    return false;
  }

  BrowseMode m = BrowseMode::COUNTRY;
  ListKind picker = ListKind::COUNTRIES;
  const char *what = "countries";
  if (tab == 1) {
    m = BrowseMode::POPULAR;
  } else if (tab == 2) {
    m = BrowseMode::TAG;
    picker = ListKind::TAGS;
    what = "categories";
  } else if (tab == 3) {
    m = BrowseMode::LANGUAGE;
    picker = ListKind::LANGUAGES;
    what = "languages";
  }

  // Tapping the tab of the list we are currently inside goes back to its
  // picker (so an accidental country selection is one tap away from undo).
  if (this->kind_ == ListKind::STATIONS && this->mode_ == m && m != BrowseMode::POPULAR) {
    this->sel_code_.clear();
    this->sel_label_.clear();
    this->show_picker_(picker, what);
    return !this->picker_loaded_(picker);
  }

  this->sel_code_.clear();
  this->sel_label_.clear();

  if (m == BrowseMode::POPULAR) {
    this->start_station_view_(BrowseMode::POPULAR, "", "Popular", 0);
    return true;  // always repull popular on tap
  }

  this->show_picker_(picker, what);
  return !this->picker_loaded_(picker);
}

bool ShRadio::row_click(int row) {
  const int i = this->row_index(row);
  if (i < 0)
    return false;

  if (this->kind_ == ListKind::STATIONS || this->kind_ == ListKind::FAVOURITES) {
    this->play(i);
    return false;
  }

  const ShRadioStation *ps = this->at_(i);
  if (ps == nullptr)
    return false;
  const ShRadioStation s = *ps;  // copy: start_station_view_ swaps the vector
  BrowseMode m;
  switch (this->kind_) {
    case ListKind::COUNTRIES:
      m = BrowseMode::COUNTRY;
      break;
    case ListKind::TAGS:
      m = BrowseMode::TAG;
      break;
    case ListKind::LANGUAGES:
      m = BrowseMode::LANGUAGE;
      break;
    default:
      return false;
  }
  ESP_LOGI(TAG, "Selected '%s' (%s) -> %ld station(s)", s.name.c_str(), s.url.c_str(), (long) s.count);
  this->start_station_view_(m, s.url.c_str(), s.name.c_str(), (int) s.count);
  return true;
}

void ShRadio::row_star_click(int row) {
  const int i = this->row_index(row);
  if (i < 0)
    return;
  if (this->kind_ != ListKind::STATIONS && this->kind_ != ListKind::FAVOURITES)
    return;
  const ShRadioStation *ps = this->at_(i);
  if (ps == nullptr)
    return;
  const ShRadioStation s = *ps;  // copy: favourites_ can reallocate
  if (s.uuid.empty())
    return;

  const int idx = this->find_fav_(s.uuid.c_str());
  if (idx >= 0) {
    this->favourites_.erase(this->favourites_.begin() + idx);
    this->status_ = "Removed from favourites";
  } else if ((int) this->favourites_.size() >= FAV_MAX) {
    this->status_ = "Favourites full";
  } else {
    this->favourites_.push_back(s);
    this->status_ = "Added to favourites";
  }
  this->save_favourites_();
  this->clamp_page_();
  this->updated_trigger_.trigger();
}

std::string ShRadio::view_report() const {
  std::string s = this->list_title();
  s += " / ";
  s += this->status();
  s += " / rows=";
  s += std::to_string((int) this->view_().size());
  s += " total=";
  s += std::to_string(this->total_rows_);
  s += " tab=";
  s += std::to_string(this->active_tab());
  s += " err=";
  s += this->fetch_error_ ? "1" : "0";
  s += " kind=";
  switch (this->kind_) {
    case ListKind::STATIONS:
      s += "stations";
      break;
    case ListKind::FAVOURITES:
      s += "favourites";
      break;
    case ListKind::COUNTRIES:
      s += "countries";
      break;
    case ListKind::TAGS:
      s += "tags";
      break;
    default:
      s += "languages";
      break;
  }
  for (int r = 0; r < 3; r++) {
    const auto *st = this->at_(this->row_index(r));
    if (st == nullptr)
      break;
    s += " | ";
    s += st->name;
  }
  if (s.size() > 240)
    s.resize(240);
  return s;
}

std::string ShRadio::favourites_summary() const {
  if (this->favourites_.empty())
    return "0 favourites";
  std::string s = std::to_string(this->favourites_.size()) + ": ";
  for (size_t i = 0; i < this->favourites_.size(); i++) {
    if (i > 0)
      s += "; ";
    s += this->favourites_[i].name;
  }
  if (s.size() > 240)
    s.resize(240);
  return s;
}

// ── hero / playback ─────────────────────────────────────────────────────────

std::string ShRadio::player_state() const {
  if (this->player_ == nullptr)
    return "offline";
  return media_player::media_player_state_to_string(this->player_->state);
}

bool ShRadio::is_playing() const {
  return this->player_ != nullptr && this->player_->state == media_player::MEDIA_PLAYER_STATE_PLAYING;
}

bool ShRadio::player_announcing() const {
  return this->player_ != nullptr &&
         this->player_->state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
}

const char *ShRadio::play_icon() const { return this->is_playing() ? "\U000F03E4" : "\U000F040A"; }

void ShRadio::play(int i) {
  const ShRadioStation *ps = this->at_(i);
  if (ps == nullptr)
    return;
  const ShRadioStation s = *ps;  // copy: a fetch may replace the window

  // Update the hero immediately, but let the debounce decide when (and how
  // often) the HA service call is actually made.
  this->playing_uuid_ = s.uuid;
  this->now_playing_ = s.name;
  this->now_url_ = s.url;
  this->now_sub_ = sub_for_(s);
  this->status_ = "Buffering...";
  if (this->pending_at_ != 0) {
    this->coalesced_++;
    ESP_LOGI(TAG, "play('%s') queued during a tap burst (%u coalesced)", s.name.c_str(),
             (unsigned) this->coalesced_);
  } else {
    ESP_LOGD(TAG, "play('%s') queued with %u ms debounce", s.name.c_str(), (unsigned) PLAY_DEBOUNCE_MS);
  }
  this->pending_index_ = i;
  this->pending_uuid_ = s.uuid;
  this->pending_at_ = millis();
  this->updated_trigger_.trigger();
}

void ShRadio::start_play_(int i) {
  const ShRadioStation *ps = this->at_(i);
  if (ps == nullptr)
    return;
  const ShRadioStation s = *ps;
  this->playing_uuid_ = s.uuid;
  this->now_playing_ = s.name;
  this->now_url_ = s.url;
  this->now_sub_ = sub_for_(s);
  this->radio_was_playing_ = false;  // a new selection must actually play first
  ESP_LOGI(TAG, "Playing '%s' [%s] codec=%s via Home Assistant %s", s.name.c_str(), s.country,
           s.codec[0] ? s.codec : "?", s.url.c_str());
  if (this->player_ == nullptr) {
    this->status_ = "No media player";
    this->updated_trigger_.trigger();
    return;
  }
  // Unconditional HA hand-off. There is no probe, no codec branch and no
  // on-device fallback: HA's ffmpeg proxy turns any source into the FLAC 48 kHz
  // this player advertises, so every station takes exactly this path.
  this->request_ha_playback_();
}

void ShRadio::request_ha_playback_() {
  this->status_ = "Playing via Home Assistant...";
  this->ha_pending_ = true;
  this->ha_at_ = millis() == 0 ? 1 : millis();
  this->updated_trigger_.trigger();
  this->ha_play_trigger_.trigger();
}

/// Jump to the page that holds `abs` when a hero prev/next leaves the resident
/// window, then play it once the window lands (see pump_async_fetch_).
void ShRadio::play_absolute_(int abs) {
  if (abs < 0)
    return;
  if (this->at_(abs) != nullptr) {
    this->play(abs);
    return;
  }
  const int pages = this->page_count();
  int page = abs / PAGE_SIZE;
  if (page >= pages)
    page = pages - 1;
  this->pending_abs_play_ = abs;
  this->page_ = page;
  this->status_ = "Loading...";
  this->updated_trigger_.trigger();  // loop() prefetch pulls the window
}

void ShRadio::set_hero_name(const std::string &n) {
  this->now_playing_ = n;
  this->now_sub_ = "font test";
  this->status_ = "Font test";
  ESP_LOGI(TAG, "Hero text test: '%s'", n.c_str());
  this->updated_trigger_.trigger();
}

bool ShRadio::row_is_playing(int row) const {
  const auto *s = this->at_(this->row_index(row));
  if (s == nullptr)
    return false;
  return !s->uuid.empty() && !this->playing_uuid_.empty() &&
         std::strcmp(s->uuid.c_str(), this->playing_uuid_.c_str()) == 0;
}

void ShRadio::toggle_play() {
  if (this->player_ == nullptr) {
    this->status_ = "No media player";
    this->updated_trigger_.trigger();
    return;
  }
  const auto st = this->player_->state;
  if (st == media_player::MEDIA_PLAYER_STATE_PLAYING) {
    this->player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_PAUSE).perform();
    this->status_ = "Paused";
  } else if (st == media_player::MEDIA_PLAYER_STATE_PAUSED) {
    this->player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_PLAY).perform();
    this->status_ = "Playing";
  } else {
    // Idle after a pause/stop: the stream is fed by HA, so ask HA again rather
    // than pointing the device at the URL.
    const auto &v = this->view_();
    int local = -1;
    for (size_t i = 0; i < v.size(); i++) {
      if (v[i].uuid.size() == this->playing_uuid_.size() &&
          std::strcmp(v[i].uuid.c_str(), this->playing_uuid_.c_str()) == 0) {
        local = (int) i;
        break;
      }
    }
    if (local >= 0) {
      this->start_play_(this->abs_from_local_(local));
      return;
    }
    if (!this->now_url_.empty()) {
      this->request_ha_playback_();
      return;
    }
    if (!v.empty()) {
      this->play(this->abs_from_local_(0));
      return;
    }
    this->status_ = "Pick a station first";
  }
  this->updated_trigger_.trigger();
}

void ShRadio::stop() {
  if (this->pending_at_ != 0) {
    this->pending_at_ = 0;
    this->pending_index_ = -1;
    this->pending_uuid_.clear();
    this->coalesced_ = 0;
  }
  this->ha_pending_ = false;
  this->pending_abs_play_ = -1;
  this->resume_pending_ = false;      // a deliberate stop must survive the next wake word
  this->radio_was_playing_ = false;
  if (this->player_ != nullptr)
    this->player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_STOP).perform();
  this->status_ = "Stopped";
  this->updated_trigger_.trigger();
}

void ShRadio::mark_interrupted() {
  // Called when the voice assistant / an announcement starts (see the radio
  // app's radio_voice_busy edge detector). Arm the resume when a station is
  // selected AND it was actually heard playing. The instantaneous state is not
  // enough: by the time this edge is seen the player may already be ANNOUNCING
  // or IDLE because TTS took the pipeline / HA dropped the proxy.
  // PAUSED is not "playing": the user paused it and expects it to stay paused.
  const bool active = this->player_ != nullptr &&
                      this->player_->state == media_player::MEDIA_PLAYER_STATE_PLAYING;
  this->resume_pending_ = !this->now_url_.empty() && (active || this->radio_was_playing_);
  ESP_LOGI(TAG, "Wake word: radio resume %s (state=%s)", this->resume_pending_ ? "armed" : "not needed",
           this->player_state().c_str());
}

void ShRadio::resume_if_interrupted() {
  if (!this->resume_pending_)
    return;
  this->resume_pending_ = false;
  if (this->now_url_.empty() || this->player_ == nullptr)
    return;
  // HA's ffmpeg proxy stopped feeding this player while the announcement ran, so
  // the media pipeline is idle: ask HA for the station again (same path as a
  // fresh tap), which also restores normal mixer volume after the unduck.
  ESP_LOGI(TAG, "Wake word done: re-requesting '%s' via Home Assistant", this->now_playing_.c_str());
  this->request_ha_playback_();
}

void ShRadio::next_station() {
  if (this->kind_ == ListKind::COUNTRIES || this->kind_ == ListKind::TAGS ||
      this->kind_ == ListKind::LANGUAGES)
    return;
  const auto &v = this->view_();
  if (v.empty())
    return;
  int local = -1;
  for (size_t i = 0; i < v.size(); i++) {
    if (std::strcmp(v[i].uuid.c_str(), this->playing_uuid_.c_str()) == 0) {
      local = (int) i;
      break;
    }
  }
  const int cur = local < 0 ? -1 : this->abs_from_local_(local);
  const int total = this->kind_ == ListKind::STATIONS ? this->total_rows_ : (int) v.size();
  int abs = cur + 1;
  if (total > 0 && abs >= total)
    abs = 0;  // wrap
  this->play_absolute_(abs < 0 ? 0 : abs);
}

void ShRadio::prev_station() {
  if (this->kind_ == ListKind::COUNTRIES || this->kind_ == ListKind::TAGS ||
      this->kind_ == ListKind::LANGUAGES)
    return;
  const auto &v = this->view_();
  if (v.empty())
    return;
  int local = -1;
  for (size_t i = 0; i < v.size(); i++) {
    if (std::strcmp(v[i].uuid.c_str(), this->playing_uuid_.c_str()) == 0) {
      local = (int) i;
      break;
    }
  }
  const int cur = local < 0 ? 0 : this->abs_from_local_(local);
  const int total = this->kind_ == ListKind::STATIONS ? this->total_rows_ : (int) v.size();
  int abs = cur - 1;
  if (abs < 0)
    abs = total > 0 ? total - 1 : 0;  // wrap to the real last station
  this->play_absolute_(abs);
}

// ── diagnostics ─────────────────────────────────────────────────────────────

void ShRadio::test_directory(const std::string &spec) {
  this->window_request_ = false;
  if (spec == "countries") {
    this->picker_valid_ = false;
    this->show_picker_(ListKind::COUNTRIES, "countries");
  } else if (spec == "tags") {
    this->picker_valid_ = false;
    this->show_picker_(ListKind::TAGS, "categories");
  } else if (spec == "languages") {
    this->picker_valid_ = false;
    this->show_picker_(ListKind::LANGUAGES, "languages");
  } else if (spec == "popular") {
    this->start_station_view_(BrowseMode::POPULAR, "", "Popular", 0);
  } else if (spec.rfind("country ", 0) == 0) {
    const std::string code = upper_ascii(spec.substr(8));
    this->start_station_view_(BrowseMode::COUNTRY, code, code, this->picker_count_for_(code));
  } else if (spec.rfind("tag ", 0) == 0) {
    const std::string tag = spec.substr(4);
    this->start_station_view_(BrowseMode::TAG, tag, tag, this->picker_count_for_(tag));
  } else if (spec.rfind("language ", 0) == 0) {
    const std::string lang = spec.substr(9);
    this->start_station_view_(BrowseMode::LANGUAGE, lang, lang, this->picker_count_for_(lang));
  } else {
    ESP_LOGW(TAG, "Unknown test directory spec '%s'", spec.c_str());
    return;
  }
  ESP_LOGI(TAG, "Test directory -> %s", spec.c_str());
  this->updated_trigger_.trigger();
}

// ── favourites persistence ──────────────────────────────────────────────────
//
// load() only fills the destination when the stored length matches exactly, so
// this uses a fixed-size image rather than a serialized string. The layout is
// deliberately unchanged from the first release so favourites stored by older
// firmware keep loading.

int ShRadio::find_fav_(const char *uuid) const {
  for (size_t i = 0; i < this->favourites_.size(); ++i) {
    if (uuid != nullptr && std::strcmp(this->favourites_[i].uuid.c_str(), uuid) == 0)
      return (int) i;
  }
  return -1;
}

void ShRadio::load_favourites_() {
  this->favourites_.clear();
  if (!this->pref_.load(&this->store_)) {
    ESP_LOGD(TAG, "No stored favourites");
    return;
  }
  if (this->store_.magic != FAV_MAGIC) {
    ESP_LOGW(TAG, "Favourite store magic mismatch; ignoring");
    return;
  }
  const uint32_t n = std::min<uint32_t>(this->store_.count, FAV_MAX);
  for (uint32_t i = 0; i < n; ++i) {
    ShRadioStation s;
    s.uuid = this->store_.items[i].uuid;
    s.name = this->store_.items[i].name;
    s.url = this->store_.items[i].url;
    if (s.uuid.empty() || s.url.empty())
      continue;
    this->favourites_.push_back(std::move(s));
  }
}

void ShRadio::save_favourites_() {
  std::memset(&this->store_, 0, sizeof(this->store_));
  this->store_.magic = FAV_MAGIC;
  this->store_.count = std::min<uint32_t>(this->favourites_.size(), FAV_MAX);
  for (uint32_t i = 0; i < this->store_.count; ++i) {
    const ShRadioStation &s = this->favourites_[i];
    std::strncpy(this->store_.items[i].uuid, s.uuid.c_str(), FAV_UUID_LEN - 1);
    std::strncpy(this->store_.items[i].name, s.name.c_str(), FAV_NAME_LEN - 1);
    std::strncpy(this->store_.items[i].url, s.url.c_str(), FAV_URL_LEN - 1);
  }
  if (this->pref_.save(&this->store_)) {
    global_preferences->sync();
    ESP_LOGI(TAG, "Saved favourites: %s", this->favourites_summary().c_str());
  } else {
    ESP_LOGW(TAG, "Failed to save favourites");
  }
}

}  // namespace sh_radio
}  // namespace esphome

#endif  // USE_ESP32
