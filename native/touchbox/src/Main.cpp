#include <gs/touchbox/Plugin.h>

GS_EXT_EXPORT int32_t gs_ext_main(const gs_ext_host* host, const gs_ext_plugin** plugin)
{
    return gs::touchbox::startPlugin(host, plugin);
}
