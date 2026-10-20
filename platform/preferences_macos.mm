#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <foobar2000/SDK/foobar2000.h>
#include "../modules/settings.h"
// Component-specific Objective-C name prevents collisions with other addons.
@interface DiscordFoobarRPCPreferencesController : NSViewController
@property(nonatomic,strong) NSButton* enabled;
@property(nonatomic,strong) NSTextField* applicationID;
@property(nonatomic,strong) NSTextField* statusLabel;
@end
@implementation DiscordFoobarRPCPreferencesController
- (void)loadView {
    NSView* view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,500,270)];
    self.view=view;
    self.enabled=[NSButton checkboxWithTitle:@"Enable Discord Rich Presence" target:nil action:nil];
    self.enabled.frame=NSMakeRect(15,225,465,24);[view addSubview:self.enabled];
    NSTextField* label=[NSTextField labelWithString:@"Custom Discord application ID (optional)"];
    label.frame=NSMakeRect(15,190,465,22);[view addSubview:label];
    self.applicationID=[[NSTextField alloc] initWithFrame:NSMakeRect(15,158,465,26)];[view addSubview:self.applicationID];
    NSTextField* help=[NSTextField wrappingLabelWithString:@"Leave blank to use the built-in application. Its ID is not shown or saved in settings. MusicBrainz supplies missing metadata and cached cover art."];
    help.frame=NSMakeRect(15,91,465,58);[view addSubview:help];
    NSButton* apply=[NSButton buttonWithTitle:@"Apply" target:self action:@selector(applySettings:)];
    apply.frame=NSMakeRect(385,52,95,28);[view addSubview:apply];
    NSButton* reset=[NSButton buttonWithTitle:@"Defaults" target:self action:@selector(resetSettings:)];
    reset.frame=NSMakeRect(280,52,95,28);[view addSubview:reset];
    self.statusLabel=[NSTextField wrappingLabelWithString:@""];self.statusLabel.frame=NSMakeRect(15,8,465,40);[view addSubview:self.statusLabel];
    auto settings=music_tracker::read_settings();
    self.enabled.state=settings.enabled?NSControlStateValueOn:NSControlStateValueOff;
    self.applicationID.stringValue=[NSString stringWithUTF8String:settings.custom_application_id.c_str()];
    self.statusLabel.stringValue=[NSString stringWithUTF8String:music_tracker::component_status().c_str()];
}
- (void)applySettings:(id)sender {
    (void)sender;
    music_tracker::Settings settings{self.enabled.state==NSControlStateValueOn,self.applicationID.stringValue.UTF8String};
    if(!music_tracker::valid_application_id(settings.custom_application_id)) {
        self.statusLabel.stringValue=@"Enter a numeric application ID, or leave it blank.";return;
    }
    music_tracker::write_settings(settings);
    self.statusLabel.stringValue=[NSString stringWithUTF8String:music_tracker::component_status().c_str()];
}
- (void)resetSettings:(id)sender {
    (void)sender;self.enabled.state=NSControlStateValueOn;self.applicationID.stringValue=@"";
    self.statusLabel.stringValue=@"Defaults selected. Click Apply to save.";
}
@end
namespace {
class MusicPreferencesPage final : public preferences_page {
public:
    service_ptr instantiate() override {return fb2k::wrapNSObject([DiscordFoobarRPCPreferencesController new]);}
    const char* get_name() override {return "discord-foobar-rpc";}
    GUID get_guid() override {return {0xb2d44603,0xaca1,0x4496,{0x8e,0xf1,0xdb,0xa6,0xec,0xc8,0x29,0x03}};}
    GUID get_parent_guid() override {return guid_tools;}
};
FB2K_SERVICE_FACTORY(MusicPreferencesPage);
}
#endif
