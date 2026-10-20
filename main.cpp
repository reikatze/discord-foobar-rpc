#include <foobar2000/SDK/foobar2000.h>
#include "modules/foobar.h"
DECLARE_COMPONENT_VERSION("discord-foobar-rpc","0.1.0",
    "Native foobar2000 music presence. MusicBrainz metadata and Cover Art Archive artwork.\nFirst-pass source; no companion application required.");
#ifdef _WIN32
VALIDATE_COMPONENT_FILENAME("foo_discord_foobar_rpc.dll");
#endif
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
namespace {
class ComponentLifecycle : public initquit {
public:
    void on_init() override {music_tracker::start_component();}
    void on_quit() override {music_tracker::stop_component();}
};
FB2K_SERVICE_FACTORY(ComponentLifecycle);
}
