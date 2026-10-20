#ifdef _WIN32
#include <helpers/foobar2000+atl.h>
#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>
#include "../modules/settings.h"
#include "resource.h"
namespace {
class PreferencesDialog : public CDialogImpl<PreferencesDialog>,public preferences_page_instance {
public:
    enum {IDD=IDD_MUSIC_PREFERENCES};
    explicit PreferencesDialog(preferences_page_callback::ptr callback):callback_(callback){}
    BEGIN_MSG_MAP_EX(PreferencesDialog)
        MSG_WM_INITDIALOG(on_init)
        COMMAND_HANDLER_EX(IDC_ENABLED,BN_CLICKED,on_change)
        COMMAND_HANDLER_EX(IDC_VERBOSE_LOGGING,BN_CLICKED,on_change)
        COMMAND_HANDLER_EX(IDC_APPLICATION_ID,EN_CHANGE,on_change)
    END_MSG_MAP()
    t_uint32 get_state() override {
        t_uint32 state=preferences_state::resettable | preferences_state::dark_mode_supported;
        if(edited()!=music_tracker::read_settings())state|=preferences_state::changed;
        return state;
    }
    HWND get_wnd() override {return m_hWnd;}
    void apply() override {
        auto settings=edited();
        if(!music_tracker::valid_application_id(settings.custom_application_id)) {
            MessageBox(L"Enter a numeric Discord application ID, or leave the field blank to use the built-in application.",L"discord-foobar-rpc",MB_OK|MB_ICONINFORMATION);return;
        }
        music_tracker::write_settings(settings);
        uSetDlgItemText(m_hWnd,IDC_STATUS,music_tracker::component_status().c_str());callback_->on_state_changed();
    }
    void reset() override {load({true,""});callback_->on_state_changed();}
private:
    preferences_page_callback::ptr callback_;
    fb2k::CDarkModeHooks dark_mode_;
    BOOL on_init(CWindow,LPARAM) {
        dark_mode_.AddDialogWithControls(m_hWnd);
        SendDlgItemMessage(IDC_APPLICATION_ID,EM_SETLIMITTEXT,20,0);
        load(music_tracker::read_settings());uSetDlgItemText(m_hWnd,IDC_STATUS,music_tracker::component_status().c_str());return TRUE;
    }
    void on_change(UINT,int,CWindow){callback_->on_state_changed();}
    void load(const music_tracker::Settings& s) {
        CheckDlgButton(IDC_ENABLED,s.enabled?BST_CHECKED:BST_UNCHECKED);CheckDlgButton(IDC_VERBOSE_LOGGING,s.verbose_logging?BST_CHECKED:BST_UNCHECKED);uSetDlgItemText(m_hWnd,IDC_APPLICATION_ID,s.custom_application_id.c_str());
    }
    music_tracker::Settings edited() {
        pfc::string8 id;uGetDlgItemText(m_hWnd,IDC_APPLICATION_ID,id);return {IsDlgButtonChecked(IDC_ENABLED)==BST_CHECKED,id.c_str(),IsDlgButtonChecked(IDC_VERBOSE_LOGGING)==BST_CHECKED};
    }
};
class PreferencesPage : public preferences_page_impl<PreferencesDialog> {
public:
    const char* get_name() override {return "discord-foobar-rpc";}
    GUID get_guid() override {return {0xb2d44603,0xaca1,0x4496,{0x8e,0xf1,0xdb,0xa6,0xec,0xc8,0x29,0x03}};}
    GUID get_parent_guid() override {return guid_tools;}
};
preferences_page_factory_t<PreferencesPage> factory;
}
#endif
