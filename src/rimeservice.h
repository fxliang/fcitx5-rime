/*
 * SPDX-FileCopyrightText: 2021~2021 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#ifndef _FCITX5_RIME_RIMESERVICE_H_
#define _FCITX5_RIME_RIMESERVICE_H_

#include <fcitx-utils/dbus/message.h>
#include <fcitx-utils/dbus/objectvtable.h>

namespace fcitx::rime {

class RimeEngine;
class RimeState;

class RimeService : public dbus::ObjectVTable<RimeService> {
public:
    RimeService(RimeEngine *engine);

    void setAsciiMode(bool asciiMode);
    bool isAsciiMode();
    void setSchema(const std::string &schema);
    std::string currentSchema();
    std::vector<std::string> listAllSchemas();
    std::string currentDataDir();
    std::vector<std::string> listDataDirs();
    bool createDataDir(const std::string &name);
    bool renameDataDir(const std::string &from, const std::string &to);
    bool deleteDataDir(const std::string &name);
    bool switchDataDir(const std::string &name);

private:
    RimeState *currentState();
    FCITX_OBJECT_VTABLE_METHOD(setAsciiMode, "SetAsciiMode", "b", "");
    FCITX_OBJECT_VTABLE_METHOD(isAsciiMode, "IsAsciiMode", "", "b");
    FCITX_OBJECT_VTABLE_METHOD(setSchema, "SetSchema", "s", "");
    FCITX_OBJECT_VTABLE_METHOD(currentSchema, "GetCurrentSchema", "", "s");
    FCITX_OBJECT_VTABLE_METHOD(listAllSchemas, "ListAllSchemas", "", "as");
    FCITX_OBJECT_VTABLE_METHOD(currentDataDir, "GetCurrentDataDir", "", "s");
    FCITX_OBJECT_VTABLE_METHOD(listDataDirs, "ListDataDirs", "", "as");
    FCITX_OBJECT_VTABLE_METHOD(createDataDir, "CreateDataDir", "s", "b");
    FCITX_OBJECT_VTABLE_METHOD(renameDataDir, "RenameDataDir", "ss", "b");
    FCITX_OBJECT_VTABLE_METHOD(deleteDataDir, "DeleteDataDir", "s", "b");
    FCITX_OBJECT_VTABLE_METHOD(switchDataDir, "SwitchDataDir", "s", "b");

    RimeEngine *engine_;
};

} // namespace fcitx::rime

#endif // _FCITX5_RIME_RIMESERVICE_H_
