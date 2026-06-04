#pragma once

#include <ISmmPlugin.h>
#include "version_gen.h"

class SilencedTracerBlocker final : public ISmmPlugin
{
public:
    bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
    bool Unload(char *error, size_t maxlen) override;

    const char *GetAuthor() override;
    const char *GetName() override;
    const char *GetDescription() override;
    const char *GetURL() override;
    const char *GetLicense() override;
    const char *GetVersion() override;
    const char *GetDate() override;
    const char *GetLogTag() override;
};

extern SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_GLOBALVARS();
