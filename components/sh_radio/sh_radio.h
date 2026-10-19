#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/automation.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/media_player/media_player.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace sh_radio {

#ifdef USE_PSRAM
/// STL allocator that keeps the directory lists in PSRAM.
///
/// Internal DRAM is the scarce resource on this board: with WiFi, the ESP-SR
/// wake-word front-end, LVGL and a playing audio pipeline up, the largest free
/// internal block drops to a few kB. Growing a std::vector of 40 directory entries
/// there aborted the device (operator new -> bad_alloc -> abort, because ESP-IDF
/// is built without exceptions), so the lists live in PSRAM instead, which has
/// megabytes free.
template<typename T> struct PsramAllocator {
  using value_type = T;
  PsramAllocator() = default;
  template<typename U> PsramAllocator(const PsramAllocator<U> &) {}  // NOLINT
  T *allocate(std::size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (p == nullptr)
      p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);  // last resort
    if (p == nullptr)
      abort();  // same outcome as the default allocator, but never hit in practice
    return static_cast<T *>(p);
  }
  void deallocate(T *p, std::size_t) { heap_caps_free(p); }
  template<typename U> bool operator==(const PsramAllocator<U> &) const { return true; }
  template<typename U> bool operator!=(const PsramAllocator<U> &) const { return false; }
};

/// std::string whose characters are allocated in PSRAM too.
///
/// The vector backing store alone is not enough: a std::string keeps its
/// characters on the default heap, and small allocations prefer internal DRAM.
/// A country's station list (hundreds of rows) plus the 241-country picker burned
/// all ~40 kB of free internal DRAM within a few pages, which made the next TLS
/// handshake fail with HTTP -1 and silently ended paging. With this allocator
/// every station/picker string lives in PSRAM.
using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
#else
using PsramString = std::string;
#endif
/// One directory entry. Used for three different things, distinguished by the
/// list it lives in:
///   * a station    -> uuid + url (playable) + country/bitrate metadata
///   * a country    -> url = ISO 3166-1 code, count = stations in it
///   * a tag/language -> url = the query value, count = stations in it
struct ShRadioStation {
  PsramString uuid;          // radio-browser stationuuid (favourites key)
  PsramString name;          // display name
  PsramString url;           // url_resolved for stations, query value for pickers
  // Small strings are fixed-size arrays on purpose: this struct is stored once
  // per directory entry, so a std::string per short field fragmented the heap
  // badly enough that an http_request response could no longer be allocated.
  char country[4]{};         // ISO 3166-1 alpha-2, uppercase (may be empty)
  char country_name[40]{};   // human readable country (stations only)
  char codec[10]{};          // radio-browser `codec` field, reported in logs only
  int16_t bitrate{0};        // kbps, 0 when unknown
  // Picker entries: number of stations the category holds. int32_t because the
  // global station total is ~58 000 (int16_t would overflow).
  int32_t count{0};
};

/// On-device internet radio directory + favourites.
///
/// The list page has five tabs, all fed live from radio-browser.info:
///   tab 0 country  (DEFAULT) -> /json/countries picker -> countrycode=CC
///   tab 1 popular             -> name ordered stations
///   tab 2 categories          -> /json/tags picker     -> tagList=<tag>
///   tab 3 languages           -> /json/languages picker-> language=<lang>
///   tab 4 favourites          -> NVS-backed
///
/// Playback is ALWAYS delegated to Home Assistant: the device fires
/// `on_ha_play`, the YAML calls `media_player.play_media` on this device's own HA
/// entity, and HA's ESPHome integration replaces the station URL with its ffmpeg
/// proxy URL, transcoding any source (AAC/AAC+/HLS/Opus/Vorbis) to the FLAC 48 kHz
/// this player advertises. There is therefore no codec probe, no "unsupported
/// codec" path, no playback watchdog and no auto-skip in this component - every
/// station takes the same path. The tradeoff is explicit: playback now needs HA.
///
/// The GET is done in this component so the response body can be read into a
/// PSRAM buffer; the shared `http_request` component keeps it in internal DRAM.
class ShRadio : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_media_player(media_player::MediaPlayer *player) { this->player_ = player; }
  void set_max_results(int n) { this->max_results_ = n; }

  /// Rows shown at once on the LVGL list page. Must match the page YAML.
  static constexpr int PAGE_SIZE = 5;
  /// Trailing-edge debounce for stream switches. Rapid taps collapse into a
  /// single HA service call instead of queueing one per row tapped.
  static constexpr uint32_t PLAY_DEBOUNCE_MS = 160;
  /// A fetch that never reports back (script restarted mid-request) must not
  /// wedge `needs_fetch()` forever.
  static constexpr uint32_t FETCH_TIMEOUT_MS = 20000;
  /// Hard ceiling on a directory request, including the worker's own retry.
  /// FETCH_TIMEOUT_MS deliberately only covers the "requested but the worker
  /// never picked it up" case (the worker is single-flight, so a late timeout
  /// must not free a request that is genuinely in flight); this one covers the
  /// worker too, so a wedged HTTP/TLS/DNS call can never leave the list page
  /// spinning forever. One attempt is bounded by HTTP_TIMEOUT_MS (8 s), the
  /// retry adds 1.5 s, so 30 s cannot cut a healthy request short.
  static constexpr uint32_t FETCH_HARD_TIMEOUT_MS = 30000;
  /// How long a "Loading ..." status may keep the spinner up when no request is
  /// actually in flight (the status is set before the 250 ms script debounce
  /// runs, so a short grace is expected). Bounded so the spinner can never
  /// outlive the reason for it.
  static constexpr uint32_t LOADING_GRACE_MS = 20000;
  /// Response body buffer. It lives in PSRAM (megabytes free) and must hold a
  /// WHOLE window: radio-browser returns the full station records, and Cyrillic
  /// names are multi-byte, so a 20-row RU window measured up to 45 869 bytes —
  /// the previous 32 768-byte cap truncated those bodies and the page then
  /// failed to parse ("Response was not a JSON array (32767 bytes)") forever.
  /// 96 kB leaves >2x margin over the worst window measured across RU/UA/CN/DE/US.
  static constexpr size_t FETCH_BUF_BYTES = 96 * 1024;
  /// HTTP/DNS/TLS budget for one directory request.
  static constexpr uint32_t HTTP_TIMEOUT_MS = 8000;
  /// Safety ceiling on the resident station window. The window is normally one
  /// server batch (`max_results_`, i.e. 4 pages / 20 rows), because the exact
  /// total is known up front and any page can be fetched by `offset`; this bound
  /// only exists so a future change cannot make the resident list unbounded.
  static constexpr int MAX_RESIDENT = 1000;
  /// How long to wait for Home Assistant's ffmpeg proxy to start feeding this
  /// player before reporting that it did not.
  static constexpr uint32_t HA_START_TIMEOUT_MS = 15000;
  /// Minimum gap between background prefetch attempts.
  static constexpr uint32_t PREFETCH_INTERVAL_MS = 1200;
  /// After a failed directory request, stop prefetching for this long.
  static constexpr uint32_t FETCH_BACKOFF_MS = 15000;

  enum class ListKind : uint8_t { STATIONS = 0, FAVOURITES, COUNTRIES, TAGS, LANGUAGES };
  enum class BrowseMode : uint8_t { POPULAR = 0, COUNTRY, TAG, LANGUAGE };

  /// Directory lists live in PSRAM (see PsramAllocator); internal DRAM cannot
  /// reliably hold them while the audio pipeline is running.
#ifdef USE_PSRAM
  using StationList = std::vector<ShRadioStation, PsramAllocator<ShRadioStation>>;
#else
  using StationList = std::vector<ShRadioStation>;
#endif

  // ── directory ────────────────────────────────────────────────────────────
  /// URL for the current view.
  std::string search_url();
  /// Kick off an asynchronous fetch of the current view. NON-BLOCKING: the
  /// blocking GET (DNS + TCP + TLS, which can take ~10 s on a bad network) runs
  /// in a worker task, and the response is parsed from loop() on the main task.
  /// Returns true when a request was started (or one is already in flight).
  bool fetch_now();
  bool fetching() const { return this->fetching_; }
  bool needs_fetch() const;

  // ── station list page ────────────────────────────────────────────────────
  int page() const { return this->page_; }
  /// Rows resident in PSRAM right now (one server batch, not the whole list).
  int list_size() const { return (int) this->view_().size(); }
  /// Total rows in the list currently shown. For station lists this is the
  /// authoritative radio-browser count fetched up front.
  int total_rows() const { return this->kind_ == ListKind::STATIONS ? this->total_rows_ : (int) this->view_().size(); }
  int page_count() const;
  std::string page_label() const;
  /// Advance one visible page. Returns true when the caller must run the fetch
  /// script (the page is not resident yet, or the neighbours need preloading).
  bool next_page();
  /// Rewind one visible page; at page 1 it WRAPS to the real last page (using
  /// the total known up front). Returns true when a fetch is needed.
  bool prev_page();
  /// Absolute index into the current view for a 0..PAGE_SIZE-1 *row slot* on
  /// the visible page, or -1 when that slot is past the end of the list or its
  /// data is not resident yet.
  int row_index(int row) const;
  /// Row accessors take the 0..PAGE_SIZE-1 row *slot* (they apply the page
  /// stride themselves) — never pass a `row_index()` result to them.
  std::string row_name(int row) const;
  std::string row_sub(int row) const;
  std::string row_flag(int row) const;
  std::string row_star(int row) const;
  bool row_star_visible() const;
  void row_star_click(int row);
  /// Play (station/favourite rows) or select (picker rows). Returns true when
  /// the caller must run the fetch script.
  bool row_click(int row);
  /// Switch tab. Returns true when the caller must run the fetch script.
  bool tab_select(int tab);
  int active_tab() const;
  std::string list_title() const;
  std::string favourites_summary() const;
  /// One-line model readout for the diagnostic text sensor: what the list page
  /// *should* be showing (title, status, row count, first rows, active tab).
  std::string view_report() const;
  /// True while the previous or next page of the current station list is not
  /// resident and should be pulled in the background (request 8: preload).
  bool prefetch_needed() const;
  /// True while the current view has no rows to draw yet and a request is in
  /// flight (or about to be). Drives the on-screen loading spinner so the row
  /// area is never an unexplained black hole.
  bool list_loading() const;
  /// True when the last directory request FAILED (or timed out) and the visible
  /// page still has no rows. Drives the on-screen error banner, so a failure is
  /// a visible message instead of a spinner that never ends.
  bool list_error() const { return this->fetch_error_ && this->row_index(0) < 0; }

  // ── hero / now playing page ──────────────────────────────────────────────
  std::string now_playing() const { return this->now_playing_; }
  std::string now_sub() const { return this->now_sub_; }
  /// Stream URL of the station currently on the hero page. Borrowed pointer so
  /// the YAML can hand it to Home Assistant's ffmpeg proxy without a copy.
  const char *now_url() const { return this->now_url_.c_str(); }
  std::string status() const { return this->status_; }
  void set_status(const std::string &s) { this->status_ = s; }
  std::string player_state() const;
  bool is_playing() const;
  /// True while the shared media player is playing an ANNOUNCEMENT (TTS). The
  /// radio app's voice-busy sensor uses it to resume a ducked/interrupted
  /// station exactly when the announcement has drained.
  bool player_announcing() const;
  const char *play_icon() const;
  void toggle_play();
  void play(int i);
  void play_row(int row) { this->play(this->row_index(row)); }
  void stop();
  /// Voice-assistant integration. The wake word ducks the mixer and HA stops
  /// feeding the ffmpeg proxy, so after the TTS announcement the radio has to be
  /// re-requested. `mark_interrupted()` (on `voice_assistant.on_listening`)
  /// remembers whether a station was playing; `resume_if_interrupted()` (at the
  /// end of `voice_assistant.on_end`, once the announcement has drained)
  /// re-issues the HA hand-off.
  void mark_interrupted();
  void resume_if_interrupted();
  void next_station();
  void prev_station();
  /// True when the row slot currently holds the station that is playing.
  bool row_is_playing(int row) const;
  /// Diagnostic: change the hero title (and nothing else) without touching
  /// playback. Used with the screenshot endpoint to prove that accented glyphs
  /// actually have ink (compare against a codepoint the font cannot have).
  void set_hero_name(const std::string &n);

  // ── diagnostics (HA test entities) ───────────────────────────────────────
  void test_directory(const std::string &spec);
  /// Test hook: make the next `n` directory GETs fail inside fetch_blocking_(),
  /// i.e. run the REAL failure bookkeeping (worker retries once, pump reports the
  /// error, spinner must clear, error banner must appear). Used to verify the
  /// failure path without an aircraft-mode switch on the router.
  void test_fail_fetches(int n) { this->test_fail_left_ = n; }

  Trigger<> *get_updated_trigger() { return &this->updated_trigger_; }
  /// Fired for EVERY station start: the YAML calls HA's `media_player.play_media`
  /// on this device's entity and HA's ffmpeg proxy sends the device FLAC 48 kHz.
  Trigger<> *get_ha_play_trigger() { return &this->ha_play_trigger_; }
  /// Fired when the previous/next page should be pulled in the background so a
  /// pager tap is instant. The YAML hooks this to the fetch script.
  Trigger<> *get_prefetch_trigger() { return &this->prefetch_trigger_; }

 protected:
  static constexpr int FAV_MAX = 12;
  static constexpr int FAV_UUID_LEN = 40;
  static constexpr int FAV_NAME_LEN = 48;
  static constexpr int FAV_URL_LEN = 200;
  static constexpr uint32_t FAV_MAGIC = 0x53485231;  // "SHR1"
  static constexpr uint32_t FAV_KEY = 0x52444631;    // "RDF1"

  struct FavEntry {
    char uuid[FAV_UUID_LEN];
    char name[FAV_NAME_LEN];
    char url[FAV_URL_LEN];
  };
  struct FavStore {
    uint32_t magic;
    uint32_t count;
    FavEntry items[FAV_MAX];
  };

  bool parse_json_buffer_(const uint8_t *data, size_t len);
  bool parse_array_(JsonArray arr);
  const StationList &view_() const;
  /// Row accessor for an ABSOLUTE row index (page*PAGE_SIZE+slot). Returns
  /// nullptr when the slot is past the end of the list or its rows are not in
  /// PSRAM yet.
  const ShRadioStation *at_(int abs) const;
  /// Convert an index into the resident `view_()` list to an absolute row index.
  int abs_from_local_(int local) const;
  /// Category count for a picker entry (stations in this country/tag/language),
  /// or 0 when that picker is not loaded.
  int picker_count_for_(const std::string &key) const;
  bool picker_loaded_(ListKind k) const;
  static std::string sub_for_(const ShRadioStation &s);
  void start_play_(int i);
  /// Play an absolute row index, loading the page that holds it first when the
  /// hero prev/next button walks past the resident window.
  void play_absolute_(int abs);
  /// Hand the current station to Home Assistant: HA transcodes anything to the
  /// FLAC this player advertises. Unconditional - there is no on-device codec
  /// path any more.
  void request_ha_playback_();
  int find_fav_(const char *uuid) const;
  void load_favourites_();
  void save_favourites_();
  void clamp_page_();
  void show_picker_(ListKind k, const char *what);
  /// Start a station-list view: resets the window and records the authoritative
  /// total for the query (0 = not known).
  void start_station_view_(BrowseMode m, const std::string &code, const std::string &label, int total);
  /// Blocking GET into `fetch_buf_` (PSRAM). Runs on the worker task only and
  /// never touches the LVGL tree or triggers automations.
  bool fetch_blocking_();
  /// Build the URL for the current view and hand it to the worker. Caller must
  /// have checked that no request is in flight.
  bool start_fetch_();
  /// Worker task body: sleeps until `fetch_now()` hands it a request.
  static void fetch_task_(void *arg);
  /// Main-loop side of the async fetch: parse a completed response or report
  /// the failure, then release the buffer.
  void pump_async_fetch_();
  /// Pages covered by one server batch (max_results_ / PAGE_SIZE).
  int window_pages_() const;
  /// True when `page` has its rows in PSRAM.
  bool window_covers_(int page) const;
  /// Arm a window fetch whose rows cover `page` and its neighbours. Returns
  /// true when the caller must run the fetch script.
  bool arm_window_(int page);

  media_player::MediaPlayer *player_{nullptr};
  StationList results_;
  StationList favourites_;
  // Only ONE picker list is kept alive at a time (countries, tags or languages):
  // caching all three cost ~20 kB of internal DRAM. Switching pickers refetches.
  StationList picker_;
  StationList empty_{};  // returned when a tab has no data of its own
  ListKind picker_kind_{ListKind::COUNTRIES};
  bool picker_valid_{false};
  int max_results_{20};
  int page_{0};
  // ── server-side paging (station lists only) ──────────────────────────────
  // `results_` is a window of resident rows starting at page `win_page_`. The
  // exact total comes from radio-browser's category counts (countries/tags/
  // languages `stationcount`, /json/stats for popular), so the pager knows the
  // real last page from the first render and any page can be fetched directly by
  // page-aligned `offset`.
  int total_rows_{0};
  int win_page_{0};
  int fetch_offset_{0};
  // Page-aligned start of the next window request (set by arm_window_).
  int window_start_page_{0};
  bool window_request_{false};
  bool stats_loaded_{false};  // /json/stats already fetched for "Popular"
  bool fetch_stats_{false};   // the in-flight request is the /json/stats object
  ListKind kind_{ListKind::COUNTRIES};
  BrowseMode mode_{BrowseMode::COUNTRY};
  std::string sel_code_{};
  std::string sel_label_{};
  bool fetching_{false};
  uint32_t fetch_started_{0};
  /// Set on every failure path (truncated body, parse error, HTTP/TLS failure,
  /// timeout) and cleared when a request starts, so the list page can show a
  /// visible error banner instead of spinning forever (see list_error()).
  bool fetch_error_{false};
  /// When the current "Loading ..." status was first seen (0 = not loading).
  /// Used by list_loading() to bound the grace period of the spinner.
  uint32_t loading_since_ms_{0};
  ListKind fetch_kind_{ListKind::STATIONS};
  std::string fetch_label_{};
  // ── async fetch plumbing ─────────────────────────────────────────────────
  // The blocking HTTP GET runs in `fetch_task_handle_` (created in setup()); its
  // result is handed back to loop() through these fields. Only the worker writes
  // fetch_buf_/fetch_len_/fetch_ok_/fetch_why_ before fetch_complete_, and only
  // the main task reads them after it, so no lock is needed.
  std::string fetch_url_{};
  char *fetch_buf_{nullptr};
  size_t fetch_len_{0};
  std::string fetch_why_{};
  bool fetch_requested_{false};
  bool fetch_complete_{false};
  bool fetch_ok_{false};
  bool fetch_task_busy_{false};
  /// A new view was selected while a request was in flight; when the current one
  /// lands, pump_async_fetch_() starts this request from the then-current view.
  bool fetch_pending_{false};
  /// Test-only: number of remaining GETs to fail on purpose (see test_fail_fetches).
  int test_fail_left_{0};
  TaskHandle_t fetch_task_handle_{nullptr};
  // Prefetch pacing/backoff (see prefetch_needed() and loop()).
  uint32_t last_prefetch_ms_{0};
  uint32_t prefetch_blocked_until_{0};
  std::string now_playing_{"Radio"};
  std::string now_sub_{"No station selected"};
  std::string now_url_{};
  std::string status_{"Idle"};
  std::string playing_uuid_{};

  // ── Home Assistant hand-off ──────────────────────────────────────────────
  // Set when the HA service call has been fired and the player has not yet
  // reported PLAYING. Purely a status/telemetry aid - there is no failure
  // watchdog and no auto-skip any more.
  bool ha_pending_{false};
  uint32_t ha_at_{0};
  /// A station was playing when the wake word interrupted it (see
  /// mark_interrupted()/resume_if_interrupted()).
  bool resume_pending_{false};
  /// True once the selected station actually reached PLAYING. The wake-word
  /// edge can arrive after the state has already moved to ANNOUNCING or IDLE
  /// (TTS takes the pipeline, HA cancels the proxy), so "was it playing when
  /// this started" cannot be read from the instantaneous state alone.
  bool radio_was_playing_{false};

  // trailing-edge play debounce
  int pending_index_{-1};
  std::string pending_uuid_{};
  uint32_t pending_at_{0};
  uint32_t coalesced_{0};
  /// Absolute row index the hero prev/next walked to while its page was not
  /// resident; played as soon as that window lands.
  int pending_abs_play_{-1};

  ESPPreferenceObject pref_;
  FavStore store_{};  // fixed-size NVS image (load() requires an exact length)
  Trigger<> updated_trigger_;
  Trigger<> ha_play_trigger_;
  Trigger<> prefetch_trigger_;
};

}  // namespace sh_radio
}  // namespace esphome

#endif  // USE_ESP32
