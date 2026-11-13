// stat_boost - three sliders on the Options screen (mods page) that rebalance
// every enemy through the host's Enemy API:
//
//   Enemy HP      multiplies every enemy's max HP   (lower = your attack hits harder)
//   Enemy Damage  multiplies the damage enemies deal (lower = your party is tougher)
//   EXP Gain      multiplies the EXP every enemy is worth
//
// These are per-type "ANY" overrides, which the host holds in memory and
// reapplies every frame to a snapshot of the enemy's shipped stats, so they never
// compound and never touch the save. The slider positions are kept in
// stat_boost.cfg in the game's save directory and reapplied on launch.

#include <rex/system/mod_plugin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "rexmod/eternalsonata_enemy_api.h"
#include "rexmod/eternalsonata_options_api.h"
#include "rexmod/eternalsonata_save_api.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

void* HostSymbol(const char* symbol) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), symbol));
#else
  return dlsym(RTLD_DEFAULT, symbol);
#endif
}

template <typename Fn>
Fn Resolve(const char* symbol) {
  return reinterpret_cast<Fn>(HostSymbol(symbol));
}

// Slider steps -> multiplier. Edit this table to change the available values.
constexpr float kMultipliers[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f,
                                  2.0f,  3.0f, 4.0f,  5.0f, 10.0f};
constexpr int kStepCount = static_cast<int>(sizeof(kMultipliers) / sizeof(kMultipliers[0]));
constexpr int kNeutralStep = 3;  // x1.0

struct Row {
  const char* label;
  int stat;          // ETERNALSONATA_ENEMY_STAT_*
  int default_step;  // used until a config file says otherwise
  std::atomic<int> step;
};

// Defaults: half-HP enemies, half-damage enemies, double EXP. Change them here.
Row g_rows[] = {
    {"Enemy HP", ETERNALSONATA_ENEMY_STAT_HP_MAX, 1, {1}},        // x0.5
    {"Enemy Damage", ETERNALSONATA_ENEMY_STAT_DAMAGE_PCT, 1, {1}},  // x0.5
    {"EXP Gain", ETERNALSONATA_ENEMY_STAT_EXP, 6, {6}},           // x2
};
constexpr int kRowCount = static_cast<int>(sizeof(g_rows) / sizeof(g_rows[0]));

EternalSonataSetEnemyTypeStatMultiplierFn g_set_multiplier = nullptr;
EternalSonataClearEnemyTypeStatFn g_clear_stat = nullptr;
EternalSonataGetSaveDirectoryFn g_get_save_dir = nullptr;

// Set when something still needs (re)applying; the tick below retries until the
// enemy system accepts the writes.
std::atomic<bool> g_pending{true};
// Set when a slider moved; the tick writes the config file, so the menu thread
// never does file I/O.
std::atomic<bool> g_dirty{false};
// Enemy ABI of the host; DAMAGE_PCT needs 3 or later.
uint32_t g_enemy_abi = 0;

int ClampStep(int s) { return s < 0 ? 0 : (s >= kStepCount ? kStepCount - 1 : s); }

bool ApplyRow(const Row& row) {
  if (!g_set_multiplier || !g_clear_stat) return false;
  if (row.stat == ETERNALSONATA_ENEMY_STAT_DAMAGE_PCT && g_enemy_abi < 3) {
    return true;  // host too old for this stat; skip it instead of retrying forever
  }
  const int step = ClampStep(row.step.load());
  int result;
  if (step == kNeutralStep) {
    // x1.0: remove the rule rather than asserting 1.0 forever.
    result = g_clear_stat(ETERNALSONATA_ENEMY_TYPE_ANY, row.stat);
    return result >= 0 || result == ETERNALSONATA_ENEMY_ERR_NO_OVERRIDE;
  }
  result = g_set_multiplier(ETERNALSONATA_ENEMY_TYPE_ANY, row.stat, kMultipliers[step]);
  return result >= 0;
}

bool ApplyAll() {
  bool ok = true;
  for (int i = 0; i < kRowCount; ++i) ok = ApplyRow(g_rows[i]) && ok;
  return ok;
}

std::string ConfigPath() {
  if (!g_get_save_dir) return {};
  char buf[1024] = {};
  if (g_get_save_dir(buf, sizeof(buf)) < 0 || !buf[0]) return {};
  return std::string(buf) + "/stat_boost.cfg";
}

void LoadConfig() {
  const std::string path = ConfigPath();
  if (path.empty()) return;
  std::ifstream in(path);
  if (!in) return;
  for (int i = 0; i < kRowCount; ++i) {
    int v;
    if (!(in >> v)) break;
    g_rows[i].step.store(ClampStep(v));
  }
}

void SaveConfig() {
  const std::string path = ConfigPath();
  if (path.empty()) return;
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  for (int i = 0; i < kRowCount; ++i) out << g_rows[i].step.load() << "\n";
}

int GetStep(void* user) {
  return g_rows[reinterpret_cast<intptr_t>(user)].step.load();
}

void SetStep(int index, void* user) {
  Row& row = g_rows[reinterpret_cast<intptr_t>(user)];
  row.step.store(ClampStep(index));
  if (!ApplyRow(row)) g_pending.store(true);
  g_dirty.store(true);
  REXLOG_INFO("[stat_boost] {} -> x{}", row.label, kMultipliers[ClampStep(index)]);
}

int StepText(int index, char* out, int out_size, void*) {
  return std::snprintf(out, static_cast<size_t>(out_size), "x%g", kMultipliers[ClampStep(index)]);
}

// Invisible dialog, only here to get a per-frame tick (same trick as low_hp_alarm).
class StatBoostDialog : public rex::ui::ImGuiDialog {
 public:
  explicit StatBoostDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (g_dirty.exchange(false)) SaveConfig();
    if (!g_pending.load()) return;
    since_try_ += io.DeltaTime;
    if (since_try_ < 0.5f) return;
    since_try_ = 0.0f;
    if (ApplyAll()) {
      g_pending.store(false);
      REXLOG_INFO("[stat_boost] enemy rules applied");
    }
  }

 private:
  float since_try_ = 0.0f;
};

class StatBoostMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<StatBoostDialog>(drawer);
  }

  void OnModuleLaunched() override {
    g_set_multiplier = Resolve<EternalSonataSetEnemyTypeStatMultiplierFn>(
        "EternalSonataSetEnemyTypeStatMultiplier");
    g_clear_stat = Resolve<EternalSonataClearEnemyTypeStatFn>("EternalSonataClearEnemyTypeStat");
    g_get_save_dir = Resolve<EternalSonataGetSaveDirectoryFn>("EternalSonataGetSaveDirectory");
    if (const auto abi = Resolve<EternalSonataEnemyAbiVersionFn>("EternalSonataEnemyAbiVersion")) {
      g_enemy_abi = abi();
    }
    const auto reg_slider =
        Resolve<EternalSonataRegisterOptionSliderFn>("EternalSonataRegisterOptionSlider");

    if (!g_set_multiplier || !g_clear_stat) {
      REXLOG_WARN("[stat_boost] host has no Enemy API; nothing to do");
      g_pending.store(false);
      return;
    }

    LoadConfig();
    ApplyAll();  // may be refused this early; the tick keeps retrying

    if (!reg_slider) {
      REXLOG_WARN("[stat_boost] host has no slider API; using the defaults, no in-game sliders");
      return;
    }
    for (intptr_t i = 0; i < kRowCount; ++i) {
      if (reg_slider(g_rows[i].label, kStepCount, &GetStep, &SetStep, &StepText,
                     reinterpret_cast<void*>(i)) < 0) {
        REXLOG_WARN("[stat_boost] slider '{}' rejected", g_rows[i].label);
      }
    }
  }

  void OnShutdown() override {
    if (g_clear_stat) {
      for (int i = 0; i < kRowCount; ++i) {
        g_clear_stat(ETERNALSONATA_ENEMY_TYPE_ANY, g_rows[i].stat);
      }
    }
  }

 private:
  std::unique_ptr<StatBoostDialog> dialog_;
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new StatBoostMod();
}
