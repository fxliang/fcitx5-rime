/*
 * SPDX-FileCopyrightText: 2017~2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rimeengine.h"
#include "notifications_public.h"
#include "rimeaction.h"
#include "rimestate.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <exception>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/fs.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx/action.h>
#include <fcitx/addoninstance.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/menu.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>
#include <filesystem>
#include <list>
#include <memory>
#include <rime_api.h>
#include <rime_levers_api.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

FCITX_DEFINE_LOG_CATEGORY(rime_log, "rime");

namespace fcitx::rime {

namespace {

// Allow notification for 60secs.
constexpr uint64_t NotificationTimeout = 60000000;

std::unordered_map<std::string, std::unordered_map<std::string, bool>>
parseAppOptions(rime_api_t *api, RimeConfig *config) {
    std::unordered_map<std::string, std::unordered_map<std::string, bool>>
        appOptions;
    RimeConfigIterator appIter;
    RimeConfigIterator optionIter;
    if (api->config_begin_map(&appIter, config, "app_options")) {
        while (api->config_next(&appIter)) {
            auto &options = appOptions[appIter.key];
            if (api->config_begin_map(&optionIter, config, appIter.path)) {
                while (api->config_next(&optionIter)) {
                    Bool value = False;
                    if (api->config_get_bool(config, optionIter.path, &value)) {
                        options[optionIter.key] = !!value;
                    }
                }
                api->config_end(&optionIter);
            }
        }
        api->config_end(&appIter);
    }
    return appOptions;
}

std::vector<std::string> getListItemPath(rime_api_t *api, RimeConfig *config,
                                         const std::string &path) {
    std::vector<std::string> paths;
    RimeConfigIterator iter;
    if (api->config_begin_list(&iter, config, path.c_str())) {
        while (api->config_next(&iter)) {
            paths.push_back(iter.path);
        }
        api->config_end(&iter);
    }
    return paths;
}

std::vector<std::string> getListItemString(rime_api_t *api, RimeConfig *config,
                                           const std::string &path) {
    std::vector<std::string> values;
    auto paths = getListItemPath(api, config, path);
    for (const auto &path : paths) {
        const auto *value = api->config_get_cstring(config, path.c_str());
        if (!value) {
            return {};
        }
        values.emplace_back(value);
    }
    return values;
}

std::string encodeSchemaSelectorItems(
    const std::vector<std::string> &selectedSchemas,
    const std::vector<std::pair<std::string, std::string>> &schemas) {
    std::unordered_set<std::string> selectedSet(selectedSchemas.begin(),
                                                selectedSchemas.end());
    std::ostringstream out;
    for (size_t i = 0; i < schemas.size(); i++) {
        const auto &[id, name] = schemas[i];
        out << (selectedSet.count(id) ? '1' : '0') << '\t' << id << '\t'
            << name;
        if (i + 1 < schemas.size()) {
            out << '\n';
        }
    }
    return out.str();
}

bool decodeSchemaSelectorItems(
    std::string_view text,
    std::vector<std::pair<bool, std::pair<std::string, std::string>>> &rows) {
    rows.clear();
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto firstTab = line.find('\t');
        if (firstTab == std::string::npos || firstTab != 1) {
            return false;
        }
        const auto secondTab = line.find('\t', firstTab + 1);
        if (secondTab == std::string::npos || secondTab == firstTab + 1) {
            return false;
        }
        const char selectedChar = line[0];
        if (selectedChar != '0' && selectedChar != '1') {
            return false;
        }
        std::string schemaId =
            line.substr(firstTab + 1, secondTab - firstTab - 1);
        std::string schemaName = line.substr(secondTab + 1);
        rows.emplace_back(
            selectedChar == '1',
            std::make_pair(std::move(schemaId), std::move(schemaName)));
    }
    return true;
}

rime_api_t *EnsureRimeApi() {
    auto *api = rime_get_api();
    if (!api) {
        throw std::runtime_error("Failed to get Rime API");
    }
    return api;
}

std::string trimAscii(std::string value) {
    auto isSpace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    };
    while (!value.empty() && isSpace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = trimAscii(line);
        if (!trimmed.empty()) {
            lines.emplace_back(std::move(trimmed));
        }
    }
    return lines;
}

std::string joinLines(const std::vector<std::string> &lines) {
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); i++) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << '\n';
        }
    }
    return out.str();
}

std::string normalizeProtocolKey(std::string_view value) {
    std::string key;
    key.reserve(value.size());
    for (const unsigned char ch : value) {
        if (ch == '_' || ch == '-' || ch == '/' || ch == ' ') {
            continue;
        }
        key.push_back(std::tolower(ch));
    }
    return key;
}

bool isDeployPath(std::string_view path) {
    const auto key = normalizeProtocolKey(path);
    return key == "deploy";
}

bool isSyncPath(std::string_view path) {
    const auto key = normalizeProtocolKey(path);
    return key == "sync" || key == "synchronize";
}

bool isProfileManagerPath(std::string_view path) {
    const auto key = normalizeProtocolKey(path);
    return key == "profilemanager" || key == "datadirmanager";
}

bool isSchemaSelectorPath(std::string_view path) {
    const auto key = normalizeProtocolKey(path);
    return key == "schemaselector";
}

std::string normalizeProfileAction(std::string_view action) {
    const auto key = normalizeProtocolKey(action);
    if (key == "create" || key == "add") {
        return "create";
    }
    if (key == "rename") {
        return "rename";
    }
    if (key == "delete" || key == "remove") {
        return "delete";
    }
    if (key == "switch" || key == "setcurrent" || key == "activate") {
        return "switch";
    }
    return std::string(action);
}

} // namespace

class IMAction : public Action {
public:
    IMAction(RimeEngine *engine) : engine_(engine) {}

    std::string shortText(InputContext *ic) const override {
        std::string result;
        auto *state = engine_->state(ic);
        if (state) {
            state->getStatus([&result](const RimeStatus &status) {
                result = status.schema_id ? status.schema_id : "";
                if (status.is_disabled) {
                    result = "\xe2\x8c\x9b";
                } else if (status.is_ascii_mode) {
                    result = "A";
                } else if (status.schema_name && status.schema_name[0] != '.') {
                    result = status.schema_name;
                } else {
                    result = "中";
                }
            });
        } else {
            result = "\xe2\x8c\x9b";
        }
        return result;
    }

    std::string longText(InputContext *ic) const override {
        std::string result;
        auto *state = engine_->state(ic);
        if (state) {
            state->getStatus([&result](const RimeStatus &status) {
                result = status.schema_name ? status.schema_name : "";
            });
        }
        return result;
    }

    std::string icon(InputContext *ic) const override {
        bool isDisabled = false;
        auto *state = engine_->state(ic);
        if (state) {
            state->getStatus([&isDisabled](const RimeStatus &status) {
                isDisabled = status.is_disabled;
            });
        }
        if (isDisabled) {
            return "fcitx_rime_disabled";
        }
        return "fcitx_rime_im";
    }

private:
    RimeEngine *engine_;
};

bool RimeEngine::firstRun_ = true;

RimeEngine::RimeEngine(Instance *instance)
    : instance_(instance), api_(EnsureRimeApi()),
      factory_([this](InputContext &ic) { return new RimeState(this, ic); }),
      sessionPool_(this, getSharedStatePolicy()) {
    if constexpr (isAndroid() || isApple()) {
        const auto &sp = StandardPaths::global();
        std::string defaultYaml =
            sp.locate(StandardPathsType::Data, "rime-data/default.yaml");
        if (defaultYaml.empty()) {
            throw std::runtime_error("Fail to locate shared data directory");
        }
        sharedDataDir_ = fcitx::fs::dirName(defaultYaml);
    } else {
        sharedDataDir_ = RIME_DATA_DIR;
    }
    imAction_ = std::make_unique<IMAction>(this);
    instance_->userInterfaceManager().registerAction("fcitx-rime-im",
                                                     imAction_.get());
    imAction_->setMenu(&schemaMenu_);
    eventDispatcher_.attach(&instance_->eventLoop());
    separatorAction_.setSeparator(true);
    instance_->userInterfaceManager().registerAction("fcitx-rime-separator",
                                                     &separatorAction_);

    schemaSelectorAction_.setIcon("fcitx_rime_schema_selector");
    schemaSelectorAction_.setShortText(_("Schema Selector"));
    // Use ExternalOption URI format so Android layer can read config parameters
    // Format:
    // fcitx://multiselect/addon/{addon}/{path}?option={option}&min={min}
    instance_->userInterfaceManager().registerAction(
        "fcitx://multiselect/addon/rime/schema-selector?option=Items&min=1",
        &schemaSelectorAction_);

    deployAction_.setIcon("fcitx_rime_deploy");
    deployAction_.setShortText(_("Deploy"));
    deployAction_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        deploy();
        auto *state = this->state(ic);
        if (state && ic->hasFocus()) {
            state->updateUI(ic, false);
        }
    });
    instance_->userInterfaceManager().registerAction("fcitx-rime-deploy",
                                                     &deployAction_);

    syncAction_.setIcon("fcitx_rime_sync");
    syncAction_.setShortText(_("Synchronize"));

    syncAction_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        sync(/*userTriggered=*/true);
        auto *state = this->state(ic);
        if (state && ic->hasFocus()) {
            state->updateUI(ic, false);
        }
    });
    instance_->userInterfaceManager().registerAction("fcitx-rime-sync",
                                                     &syncAction_);
    schemaMenu_.addAction(&separatorAction_);
    schemaMenu_.addAction(&schemaSelectorAction_);
    schemaMenu_.addAction(&deployAction_);
    schemaMenu_.addAction(&syncAction_);
    globalConfigReloadHandle_ = instance_->watchEvent(
        EventType::GlobalConfigReloaded, EventWatcherPhase::Default,
        [this](Event &) { refreshSessionPoolPolicy(); });

    loadDataDirState();
    allowNotification("failure");
    reloadConfig();
    constructed_ = true;
}

RimeEngine::~RimeEngine() {
    factory_.unregister();
    try {
        RIME_INFO() << "RimeEngine dtor: finalize()";
        api_->finalize();
    } catch (const std::exception &e) {
        RIME_ERROR() << e.what();
    }
}

void RimeEngine::rimeStart(bool fullcheck) {
    RIME_INFO() << "Rime Start: fullcheck=" << fullcheck
                << ", currentDataDir=" << currentDataDir_;

    auto userDir =
        StandardPaths::global().userDirectory(StandardPathsType::PkgData) /
        currentDataDir_;
    RIME_DEBUG() << "Rime data directory: " << userDir;
    if (!fs::makePath(userDir)) {
        if (!fs::isdir(userDir)) {
            RIME_ERROR() << "Failed to create user directory: " << userDir;
        }
    }

    RIME_STRUCT(RimeTraits, fcitx_rime_traits);
    fcitx_rime_traits.shared_data_dir = sharedDataDir_.c_str();
    fcitx_rime_traits.app_name = "rime.fcitx-rime";
    fcitx_rime_traits.user_data_dir = userDir.c_str();
    fcitx_rime_traits.distribution_name = "Rime";
    fcitx_rime_traits.distribution_code_name = "fcitx-rime";
    fcitx_rime_traits.distribution_version = FCITX_RIME_VERSION;
    std::string logDir;
    if (*config_.logToStderr) {
        // Empty log_dir tells librime to print logs to stderr.
        fcitx_rime_traits.log_dir = "";
    } else {
        auto logDirPath = userDir / "log";
        if (!fs::makePath(logDirPath) && !fs::isdir(logDirPath)) {
            RIME_ERROR() << "Failed to create rime log directory: "
                         << logDirPath << ", falling back to stderr.";
            fcitx_rime_traits.log_dir = "";
        } else {
            logDir = logDirPath.string();
            fcitx_rime_traits.log_dir = logDir.c_str();
            RIME_DEBUG() << "Rime log directory: " << logDirPath;
        }
    }
    switch (rime_log().logLevel()) {
    case NoLog:
        fcitx_rime_traits.min_log_level = 4;
        break;
    case Fatal:
        fcitx_rime_traits.min_log_level = 3;
        break;
    case Error:
    case Warn:
    case Info:
        fcitx_rime_traits.min_log_level = 2;
        break;
    case Debug:
    default:
        // Rime info is too noisy.
        fcitx_rime_traits.min_log_level = 0;
        break;
    }

    fcitx_rime_traits.modules = nullptr;

    if (firstRun_) {
        RIME_INFO() << "Rime Start: firstRun setup()";
        api_->setup(&fcitx_rime_traits);
        firstRun_ = false;
    }
    RIME_INFO() << "Rime Start: initialize()";
    api_->initialize(&fcitx_rime_traits);
    api_->set_notification_handler(&rimeNotificationHandler, this);
    api_->start_maintenance(fullcheck);
    RIME_INFO() << "Rime Start: maintenanceMode="
                << api_->is_maintenance_mode();

    if (!api_->is_maintenance_mode()) {
        updateAppOptions();
    } else {
        needRefreshAppOption_ = true;
    }
}

void RimeEngine::updateAppOptions() {
    appOptions_.clear();
    RimeConfig config = {nullptr};
    if (api_->config_open("fcitx5", &config)) {
        appOptions_ = parseAppOptions(api_, &config);
        api_->config_close(&config);
    }
    RIME_DEBUG() << "App options are " << appOptions_;
    releaseAllSession();
}

void RimeEngine::reloadConfig() {
    RIME_INFO() << "Rime reloadConfig()";
    readAsIni(config_, "conf/rime.conf");
    updateConfig();
}

void RimeEngine::setSubConfig(const std::string &path,
                              const RawConfig &config) {
    if (isDeployPath(path)) {
        deploy();
    } else if (isSyncPath(path)) {
        sync(/*userTriggered=*/true);
    } else if (isProfileManagerPath(path)) {
        const auto actionCfg = config.get("Action");
        if (!actionCfg) {
            dataDirLastError_ = "Missing Action field.";
            return;
        }
        const auto action = normalizeProfileAction(actionCfg->value());
        const std::string name =
            config.get("Name") ? config.get("Name")->value() : "";
        const std::string newName =
            config.get("NewName") ? config.get("NewName")->value() : "";
        std::string error;
        bool ok = false;
        if (action == "create") {
            ok = createDataDir(name, &error);
        } else if (action == "rename") {
            ok = renameDataDir(name, newName, &error);
        } else if (action == "delete") {
            ok = deleteDataDir(name, &error);
        } else if (action == "switch") {
            ok = switchDataDir(name, /*fullcheck=*/true, &error);
        } else {
            error = "Unknown action: " + action;
        }
        if (!ok) {
            dataDirLastError_ = error;
            RIME_ERROR() << "Data directory operation failed: action=" << action
                         << ", error=" << error;
        } else {
            dataDirLastError_.clear();
        }
    } else if (isSchemaSelectorPath(path)) {
        std::vector<std::pair<bool, std::pair<std::string, std::string>>> rows;
        if (const auto itemsConfig = config.get("Items");
            !itemsConfig ||
            !decodeSchemaSelectorItems(itemsConfig->value(), rows)) {
            RIME_ERROR() << "Schema selector update ignored: bad item payload.";
            return;
        }
        RIME_DEBUG() << "Schema selector update payload rows=" << rows.size();
        std::vector<std::string> selected;
        selected.reserve(rows.size());
        for (const auto &row : rows) {
            if (row.first) {
                selected.push_back(row.second.first);
            }
        }
        if (selected.empty()) {
            RIME_ERROR() << "Schema selector update ignored: empty selection.";
            return;
        }

        auto *module = api_->find_module("levers");
        if (!module || !module->get_api) {
            RIME_ERROR()
                << "Schema selector update ignored: missing levers module.";
            return;
        }
        auto *leversApi = reinterpret_cast<RimeLeversApi *>(module->get_api());
        if (!leversApi || !leversApi->switcher_settings_init ||
            !leversApi->load_settings || !leversApi->save_settings ||
            !leversApi->select_schemas || !leversApi->custom_settings_destroy) {
            RIME_ERROR()
                << "Schema selector update ignored: levers API unavailable.";
            return;
        }
        auto *settings = leversApi->switcher_settings_init();
        if (!settings) {
            RIME_ERROR() << "Schema selector update ignored: failed to init "
                            "switcher settings.";
            return;
        }
        auto *customSettings = reinterpret_cast<RimeCustomSettings *>(settings);
        if (!leversApi->load_settings(customSettings)) {
            // Some fresh Android profiles may not have switcher settings yet.
            // Continue so select_schemas/save_settings can create them.
            RIME_ERROR() << "Schema selector update: load_settings returned "
                            "false, continue with defaults.";
        }
        std::vector<const char *> schemaIds;
        schemaIds.reserve(selected.size());
        for (const auto &id : selected) {
            schemaIds.push_back(id.c_str());
        }
        const Bool ok = leversApi->select_schemas(settings, schemaIds.data(),
                                                  schemaIds.size());
        if (ok && !leversApi->save_settings(customSettings)) {
            leversApi->custom_settings_destroy(customSettings);
            RIME_ERROR() << "Schema selector update failed: save_settings "
                            "returned false.";
            return;
        }
        leversApi->custom_settings_destroy(customSettings);
        if (!ok) {
            RIME_ERROR() << "Schema selector update failed: select_schemas "
                            "returned false.";
            return;
        }
        RIME_DEBUG() << "Schema selector update selected=" << selected.size();

        allowNotification();
        deploy();
    }
}

const Configuration *RimeEngine::getSubConfig(const std::string &path) const {
    if (isProfileManagerPath(path)) {
        dataDirManagerConfig_.current.setValue(currentDataDir_);
        dataDirManagerConfig_.profiles.setValue(joinLines(dataDirs_));
        dataDirManagerConfig_.error.setValue(dataDirLastError_);
        return &dataDirManagerConfig_;
    }

    if (!isSchemaSelectorPath(path)) {
        return nullptr;
    }
    auto *module = api_->find_module("levers");
    if (!module || !module->get_api) {
        return nullptr;
    }
    auto *leversApi = reinterpret_cast<RimeLeversApi *>(module->get_api());
    if (!leversApi || !leversApi->switcher_settings_init ||
        !leversApi->load_settings || !leversApi->get_available_schema_list ||
        !leversApi->get_selected_schema_list ||
        !leversApi->schema_list_destroy ||
        !leversApi->custom_settings_destroy) {
        RIME_ERROR() << "Schema selector getSubConfig unavailable: levers API "
                        "incomplete.";
        return nullptr;
    }
    auto *settings = leversApi->switcher_settings_init();
    if (!settings) {
        RIME_ERROR() << "Schema selector getSubConfig failed: "
                        "switcher_settings_init returned null.";
        return nullptr;
    }
    auto *customSettings = reinterpret_cast<RimeCustomSettings *>(settings);
    if (!leversApi->load_settings(customSettings)) {
        // Fresh profiles may not have switcher settings generated yet.
        // Continue and let API return whatever is currently available.
        RIME_ERROR() << "Schema selector getSubConfig: load_settings returned "
                        "false, continue with defaults.";
    }

    RimeSchemaList available{};
    RimeSchemaList selected{};
    std::vector<std::pair<std::string, std::string>> rows;
    std::vector<std::string> selectedIds;
    if (leversApi->get_selected_schema_list(settings, &selected)) {
        selectedIds.reserve(selected.size);
        for (size_t i = 0; i < selected.size; i++) {
            const auto *id = selected.list[i].schema_id;
            if (id) {
                selectedIds.emplace_back(id);
            }
        }
        leversApi->schema_list_destroy(&selected);
    }
    if (leversApi->get_available_schema_list(settings, &available)) {
        std::unordered_set<std::string> selectedSet(selectedIds.begin(),
                                                    selectedIds.end());
        rows.reserve(available.size);
        for (size_t i = 0; i < available.size; i++) {
            const auto *id = available.list[i].schema_id;
            const auto *name = available.list[i].name;
            if (id && name && selectedSet.count(id)) {
                rows.emplace_back(id, name);
            }
        }
        for (size_t i = 0; i < available.size; i++) {
            const auto *id = available.list[i].schema_id;
            const auto *name = available.list[i].name;
            if (id && name && !selectedSet.count(id)) {
                rows.emplace_back(id, name);
            }
        }
        leversApi->schema_list_destroy(&available);
    }
    leversApi->custom_settings_destroy(customSettings);
    RIME_DEBUG() << "Schema selector getSubConfig selected="
                 << selectedIds.size() << " available=" << rows.size();
    schemaSelectorConfig_.items.setValue(
        encodeSchemaSelectorItems(selectedIds, rows));
    return &schemaSelectorConfig_;
}

void RimeEngine::updateConfig() {
    RIME_INFO() << "Rime updateConfig(): restartRime(fullcheck=false)";
    restartRime(false);
}

void RimeEngine::restartRime(bool fullcheck) {
    RIME_INFO() << "Rime restartRime(): begin, fullcheck=" << fullcheck
                << ", constructed=" << constructed_
                << ", factoryRegistered=" << factory_.registered();
    if (constructed_ && factory_.registered()) {
        releaseAllSession(true);
    }
    try {
        RIME_INFO() << "Rime restartRime(): finalize()";
        api_->finalize();
    } catch (const std::exception &e) {
        RIME_ERROR() << e.what();
    }

    rimeStart(fullcheck);
    RIME_INFO() << "Rime restartRime(): re-register input context property";
    instance_->inputContextManager().registerProperty("rimeState", &factory_);
    updateSchemaMenu();
    refreshSessionPoolPolicy();

    deployAction_.setHotkey(config_.deploy.value());
    syncAction_.setHotkey(config_.synchronize.value());

    if (constructed_) {
        refreshStatusArea(0);
    }
    RIME_INFO() << "Rime restartRime(): end";
}

void RimeEngine::refreshStatusArea(InputContext &ic) {
    // prevent modifying status area owned by other ime
    // e.g. keyboard-us when typing password
    if (instance_->inputMethod(&ic) != "rime") {
        return;
    }
    auto &statusArea = ic.statusArea();
    statusArea.clearGroup(StatusGroup::InputMethod);
    statusArea.addAction(StatusGroup::InputMethod, imAction_.get());

    auto *rimeState = state(&ic);
    std::string currentSchema;
    if (!rimeState) {
        return;
    }
    rimeState->getStatus([&currentSchema](const RimeStatus &status) {
        currentSchema = status.schema_id ? status.schema_id : "";
    });
    if (currentSchema.empty()) {
        return;
    }

    if (auto iter = optionActions_.find(currentSchema);
        iter != optionActions_.end()) {
        for (const auto &action : iter->second) {
            statusArea.addAction(StatusGroup::InputMethod, action.get());
        }
    }
}

void RimeEngine::refreshStatusArea(RimeSessionId session) {
    instance_->inputContextManager().foreachFocused(
        [this, session](InputContext *ic) {
            if (auto *state = this->state(ic)) {
                // After a deployment, param is 0, refresh all
                if (!session || state->session(false) == session) {
                    refreshStatusArea(*ic);
                }
            }
            return true;
        });
}

void RimeEngine::updateStatusArea(RimeSessionId session) {
    instance_->inputContextManager().foreachFocused(
        [this, session](InputContext *ic) {
            if (instance_->inputMethod(ic) != "rime") {
                return true;
            }
            if (auto *state = this->state(ic)) {
                // After a deployment, param is 0, refresh all
                if (!session || state->session(false) == session) {
                    // Re-read new option values.
                    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
                }
            }
            return true;
        });
}

void RimeEngine::activate(const InputMethodEntry & /*entry*/,
                          InputContextEvent &event) {
    auto *ic = event.inputContext();
    refreshStatusArea(*ic);
    if (auto *state = this->state(ic)) {
        state->activate();
    }
}

void RimeEngine::deactivate(const InputMethodEntry &entry,
                            InputContextEvent &event) {
    if (event.type() == EventType::InputContextSwitchInputMethod) {
        auto *inputContext = event.inputContext();
        auto *state = this->state(inputContext);
        switch (*config_.switchInputMethodBehavior) {
        case SwitchInputMethodBehavior::Clear:
            break;
        case SwitchInputMethodBehavior::CommitRawInput:
            state->commitInput(inputContext);
            break;
        case SwitchInputMethodBehavior::CommitComposingText:
            state->commitComposing(inputContext);
            break;
        case SwitchInputMethodBehavior::CommitCommitPreview:
            state->commitPreedit(inputContext);
            break;
        }
    }
    reset(entry, event);
}

void RimeEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &event) {
    FCITX_UNUSED(entry);
    RIME_DEBUG() << "Rime receive key: " << event.rawKey() << " "
                 << event.isRelease();
    auto *inputContext = event.inputContext();
    if (!event.isRelease()) {
        if (event.key().checkKeyList(*config_.deploy)) {
            deploy();
            event.filterAndAccept();
            return;
        }
        if (event.key().checkKeyList(*config_.synchronize)) {
            sync(/*userTriggered=*/true);
            event.filterAndAccept();
            return;
        }
    }
    auto *state = this->state(inputContext);
    currentKeyEventState_ = state;
    state->keyEvent(event);
    currentKeyEventState_ = nullptr;
}

void RimeEngine::reset(const InputMethodEntry & /*entry*/,
                       InputContextEvent &event) {
    auto *inputContext = event.inputContext();
    auto *state = this->state(inputContext);
    state->clear();
    instance_->resetCompose(inputContext);
    inputContext->inputPanel().reset();
    inputContext->updatePreedit();
    inputContext->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void RimeEngine::allowNotification(std::string type) {
    allowNotificationUntil_ = now(CLOCK_MONOTONIC) + NotificationTimeout;
    allowNotificationType_ = std::move(type);
}

void RimeEngine::save() { sync(/*userTriggered=*/false); }

void RimeEngine::rimeNotificationHandler(void *context, RimeSessionId session,
                                         const char *messageType,
                                         const char *messageValue) {
    RIME_DEBUG() << "Notification: " << session << " " << messageType << " "
                 << messageValue;
    auto *that = static_cast<RimeEngine *>(context);
    if (that->mainThreadId_ == std::this_thread::get_id()) {
        that->notifyImmediately(session, messageType, messageValue);
    }
    that->eventDispatcher_.schedule(
        [that, session, messageType = std::string(messageType),
         messageValue = std::string(messageValue)]() {
            that->notify(session, messageType, messageValue);
        });
}

void RimeEngine::notifyImmediately(RimeSessionId session,
                                   std::string_view messageType,
                                   std::string_view messageValue) {
    if (messageType != "option") {
        return;
    }
    if (!currentKeyEventState_ ||
        currentKeyEventState_->session(false) != session) {
        return;
    }
    currentKeyEventState_->addChangedOption(messageValue);
}

void RimeEngine::notify(RimeSessionId session, const std::string &messageType,
                        const std::string &messageValue) {
    const char *message = nullptr;
    const char *icon = "";
    const char *tipId = "";
    const int timeout = 3000;
    bool blockMessage = false;
    if (messageType == "deploy") {
        tipId = "fcitx-rime-deploy";
        icon = "fcitx_rime_deploy";
        if (messageValue == "start") {
            message = _("Rime is under maintenance. It may take a few "
                        "seconds. Please wait until it is finished...");
        } else if (messageValue == "success") {
            message = _("Rime is ready.");
            if (!api_->is_maintenance_mode()) {
                if (needRefreshAppOption_) {
                    api_->deploy_config_file("fcitx5.yaml", "config_version");
                    updateAppOptions();
                    needRefreshAppOption_ = false;
                }
            }
            updateSchemaMenu();
            refreshStatusArea(0);
            blockMessage = true;
        } else if (messageValue == "failure") {
            needRefreshAppOption_ = false;
            message = _("Rime has encountered an error. "
                        "See log for details.");
            blockMessage = true;
        }
    } else if (messageType == "option") {
        updateStatusArea(session);
    } else if (messageType == "schema") {
        // Schema is changed either via status area or shortcut
        refreshStatusArea(session);
    }

    auto *notifications = this->notifications();
    const auto current = now(CLOCK_MONOTONIC);
    if (message && notifications && current > silenceNotificationUntil_ &&
        (current < allowNotificationUntil_ &&
         (allowNotificationType_.empty() ||
          messageType == allowNotificationType_))) {
        notifications->call<INotifications::showTip>(
            tipId, _("Rime"), icon, _("Rime"), message, timeout);
    }
    // Block message after error / success.
    if (blockMessage) {
        silenceNotificationUntil_ = current + 30000;
    }
}

RimeState *RimeEngine::state(InputContext *ic) {
    if (!factory_.registered()) {
        return nullptr;
    }
    return ic->propertyFor(&factory_);
}

std::string RimeEngine::subMode(const InputMethodEntry & /*entry*/,
                                InputContext &ic) {
    if (auto *rimeState = state(&ic)) {
        return rimeState->subMode();
    }
    return "";
}

std::string RimeEngine::subModeLabelImpl(const InputMethodEntry & /*unused*/,
                                         InputContext &ic) {
    if (auto *rimeState = state(&ic)) {
        return rimeState->subModeLabel();
    }
    return "";
}

std::string RimeEngine::subModeIconImpl(const InputMethodEntry & /*unused*/,
                                        InputContext &ic) {
    std::string result = "fcitx-rime";
    if (!factory_.registered()) {
        return result;
    }
    auto *state = this->state(&ic);
    if (state) {
        state->getStatus([&result](const RimeStatus &status) {
            if (status.is_disabled) {
                result = "fcitx_rime_disable";
            } else if (status.is_ascii_mode) {
                result = "fcitx_rime_latin";
            } else {
                result = "fcitx-rime";
            }
        });
    }
    return result;
}

void RimeEngine::releaseAllSession(bool snapshot) {
    instance_->inputContextManager().foreach([&](InputContext *ic) {
        if (auto *state = this->state(ic)) {
            if (snapshot) {
                state->snapshot();
            }
            state->release();
        }
        return true;
    });
}

void RimeEngine::deploy() {
    RIME_INFO() << "Rime deploy(): restartRime(fullcheck=true)";
    allowNotification();
    restartRime(true);
}

void RimeEngine::sync(bool userTriggered) {
    RIME_DEBUG() << "Rime Sync user data";
    releaseAllSession(true);
    if (userTriggered) {
        allowNotification();
    }
    api_->sync_user_data();
}

void RimeEngine::updateActionsForSchema(const std::string &schema) {
    RimeConfig config{};

    if (!api_->schema_open(schema.c_str(), &config)) {
        return;
    }
    auto switchPaths = getListItemPath(api_, &config, "switches");
    for (const auto &switchPath : switchPaths) {
        auto labels = getListItemString(api_, &config, switchPath + "/states");
        if (labels.size() <= 1) {
            continue;
        }
        auto namePath = switchPath + "/name";
        const auto *name = api_->config_get_cstring(&config, namePath.c_str());
        if (name) {
            if (labels.size() != 2) {
                continue;
            }
            std::string optionName = name;
            // if (optionName == RIME_ASCII_MODE) {
            //     // imAction_ has latin mode that does the same
            //     continue;
            // }

            optionActions_[schema].emplace_back(std::make_unique<ToggleAction>(
                this, schema, optionName, labels[0], labels[1]));
        } else {
            auto options =
                getListItemString(api_, &config, switchPath + "/options");
            if (labels.size() != options.size()) {
                continue;
            }
            optionActions_[schema].emplace_back(
                std::make_unique<SelectAction>(this, schema, options, labels));
        }
    }
    api_->config_close(&config);
}

void RimeEngine::updateSchemaMenu() {
    schemas_.clear();
    schemActions_.clear();
    optionActions_.clear();
    RimeSchemaList list;
    list.size = 0;
    if (api_->get_schema_list(&list)) {
        schemActions_.emplace_back();

        schemActions_.back().setShortText(_("Latin Mode"));
        schemActions_.back().connect<SimpleAction::Activated>(
            [this](InputContext *ic) {
                auto *state = this->state(ic);
                state->toggleLatinMode();
                imAction_->update(ic);
            });
        instance_->userInterfaceManager().registerAction(&schemActions_.back());
        schemaMenu_.insertAction(&separatorAction_, &schemActions_.back());
        for (size_t i = 0; i < list.size; i++) {
            schemActions_.emplace_back();
            std::string schemaId = list.list[i].schema_id;
            auto &schemaAction = schemActions_.back();
            schemaAction.setShortText(list.list[i].name);
            schemaAction.connect<SimpleAction::Activated>(
                [this, schemaId](InputContext *ic) {
                    auto *state = this->state(ic);
                    state->selectSchema(schemaId);
                    imAction_->update(ic);
                });
            instance_->userInterfaceManager().registerAction(&schemaAction);
            schemaMenu_.insertAction(&separatorAction_, &schemaAction);
            updateActionsForSchema(schemaId);
            schemas_.insert(schemaId);
        }
        api_->free_schema_list(&list);
    }
}

void RimeEngine::refreshSessionPoolPolicy() {
    auto newPolicy = getSharedStatePolicy();
    if (sessionPool_.propertyPropagatePolicy() != newPolicy) {
        releaseAllSession(constructed_);
        sessionPool_.setPropertyPropagatePolicy(newPolicy);
    }
}

std::string RimeEngine::dataRootDir() const {
    return StandardPaths::global()
        .userDirectory(StandardPathsType::PkgData)
        .string();
}

void RimeEngine::loadDataDirState() {
    readAsIni(dataDirStateConfig_, "conf/rime-data-dir.conf");

    dataDirs_.clear();
    const auto parsed = splitLines(*dataDirStateConfig_.profiles);
    for (const auto &name : parsed) {
        if (std::find(dataDirs_.begin(), dataDirs_.end(), name) ==
            dataDirs_.end()) {
            dataDirs_.push_back(name);
        }
    }
    if (dataDirs_.empty()) {
        dataDirs_.push_back("rime");
    }

    auto current = trimAscii(*dataDirStateConfig_.current);
    if (current.empty()) {
        current = "rime";
    }
    if (std::find(dataDirs_.begin(), dataDirs_.end(), current) ==
        dataDirs_.end()) {
        dataDirs_.push_back(current);
    }
    currentDataDir_ = std::move(current);

    std::string ignored;
    ensureDataDirExists(currentDataDir_, &ignored);
    saveDataDirState();
}

void RimeEngine::saveDataDirState() {
    dataDirStateConfig_.current.setValue(currentDataDir_);
    dataDirStateConfig_.profiles.setValue(joinLines(dataDirs_));
    safeSaveAsIni(dataDirStateConfig_, "conf/rime-data-dir.conf");
}

bool RimeEngine::validateDataDirName(const std::string &name,
                                     std::string *error) const {
    if (name.empty()) {
        if (error) {
            *error = "Directory name must not be empty.";
        }
        return false;
    }
    if (name.size() > 64) {
        if (error) {
            *error = "Directory name is too long (max 64).";
        }
        return false;
    }
    if (name == "." || name == "..") {
        if (error) {
            *error = "Invalid directory name.";
        }
        return false;
    }
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        if (error) {
            *error = "Directory name must not contain path separators.";
        }
        return false;
    }
    for (const unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f) {
            if (error) {
                *error = "Directory name contains invalid control characters.";
            }
            return false;
        }
    }
    return true;
}

bool RimeEngine::ensureDataDirExists(const std::string &name,
                                     std::string *error) const {
    if (!validateDataDirName(name, error)) {
        return false;
    }
    const auto dir = std::filesystem::path(dataRootDir()) / name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error) {
            *error = "Failed to create directory: " + dir.string();
        }
        return false;
    }
    return true;
}

std::vector<std::string> RimeEngine::listDataDirs() const { return dataDirs_; }

bool RimeEngine::createDataDir(const std::string &name, std::string *error) {
    if (!validateDataDirName(name, error)) {
        return false;
    }
    if (std::find(dataDirs_.begin(), dataDirs_.end(), name) !=
        dataDirs_.end()) {
        if (error) {
            *error = "Directory already exists.";
        }
        return false;
    }
    if (!ensureDataDirExists(name, error)) {
        return false;
    }
    dataDirs_.push_back(name);
    saveDataDirState();
    return true;
}

bool RimeEngine::renameDataDir(const std::string &from, const std::string &to,
                               std::string *error) {
    if (!validateDataDirName(from, error) || !validateDataDirName(to, error)) {
        return false;
    }
    auto fromIter = std::find(dataDirs_.begin(), dataDirs_.end(), from);
    if (fromIter == dataDirs_.end()) {
        if (error) {
            *error = "Source directory does not exist.";
        }
        return false;
    }
    if (std::find(dataDirs_.begin(), dataDirs_.end(), to) != dataDirs_.end()) {
        if (error) {
            *error = "Target directory already exists.";
        }
        return false;
    }

    const auto root = std::filesystem::path(dataRootDir());
    const auto fromPath = root / from;
    const auto toPath = root / to;
    std::error_code ec;
    if (std::filesystem::exists(fromPath, ec)) {
        ec.clear();
        std::filesystem::rename(fromPath, toPath, ec);
        if (ec) {
            if (error) {
                *error = "Failed to rename directory.";
            }
            return false;
        }
    }

    *fromIter = to;
    const bool renamedCurrent = currentDataDir_ == from;
    if (renamedCurrent) {
        currentDataDir_ = to;
    }
    saveDataDirState();

    if (renamedCurrent) {
        restartRime(true);
    }
    return true;
}

bool RimeEngine::deleteDataDir(const std::string &name, std::string *error) {
    if (!validateDataDirName(name, error)) {
        return false;
    }
    auto iter = std::find(dataDirs_.begin(), dataDirs_.end(), name);
    if (iter == dataDirs_.end()) {
        if (error) {
            *error = "Directory does not exist.";
        }
        return false;
    }
    if (dataDirs_.size() <= 1) {
        if (error) {
            *error = "At least one data directory must remain.";
        }
        return false;
    }
    if (name == currentDataDir_) {
        if (error) {
            *error = "Cannot delete current directory. Please switch first.";
        }
        return false;
    }

    const auto dir = std::filesystem::path(dataRootDir()) / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        if (error) {
            *error = "Failed to delete directory.";
        }
        return false;
    }
    dataDirs_.erase(iter);
    if (dataDirs_.empty()) {
        dataDirs_.push_back("rime");
    }
    saveDataDirState();
    return true;
}

bool RimeEngine::switchDataDir(const std::string &name, bool fullcheck,
                               std::string *error) {
    if (!validateDataDirName(name, error)) {
        return false;
    }
    if (std::find(dataDirs_.begin(), dataDirs_.end(), name) ==
        dataDirs_.end()) {
        if (error) {
            *error = "Directory does not exist.";
        }
        return false;
    }
    if (!ensureDataDirExists(name, error)) {
        return false;
    }
    if (name == currentDataDir_) {
        return true;
    }
    currentDataDir_ = name;
    saveDataDirState();
    allowNotification();
    restartRime(fullcheck);
    return true;
}

PropertyPropagatePolicy RimeEngine::getSharedStatePolicy() {
    switch (*config_.sharedStatePolicy) {
    case SharedStatePolicy::All:
        return PropertyPropagatePolicy::All;
    case SharedStatePolicy::Program:
        return PropertyPropagatePolicy::Program;
    case SharedStatePolicy::No:
        return PropertyPropagatePolicy::No;
    case SharedStatePolicy::FollowGlobalConfig:
    default:
        return instance_->globalConfig().shareInputState();
    }
}

bool RimeEngine::supportsAltTrigger() const {
    // Default: disable Fcitx Shift_L toggle (let Rime handle it)
    switch (*config_.shiftKeyBehavior) {
    case ShiftKeyBehavior::EnableFcitxToggle:
        return true; // Let Fcitx handle Shift_L
    case ShiftKeyBehavior::DisableFcitxToggle:
        return false; // Rime handles Shift_L
    case ShiftKeyBehavior::Auto:
    default:
        // Auto mode: disable Fcitx toggle if Rime has Chinese/English switch
        // For now, default to disabling Fcitx toggle
        return false;
    }
}

} // namespace fcitx::rime
