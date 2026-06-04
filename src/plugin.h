#pragma once

#include <ISmmPlugin.h>
#include "version_gen.h"

class SilencedTracerBlocker final : public ISmmPlugin
{
public:
    bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
    bool Unload(char *error, size_t maxlen) override;

    const char *GetAuthor() override { return PLUGIN_AUTHOR; }
    const char *GetName() override { return PLUGIN_DISPLAY_NAME; }
    const char *GetDescription() override { return PLUGIN_DESCRIPTION; }
    const char *GetURL() override { return PLUGIN_URL; }
    const char *GetLicense() override { return PLUGIN_LICENSE; }
    const char *GetVersion() override { return PLUGIN_FULL_VERSION; }
    const char *GetDate() override { return __DATE__; }
    const char *GetLogTag() override { return PLUGIN_LOGTAG; }
};

extern SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_GLOBALVARS();
