// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.

#include "brave/browser/brave_rewards/add_funds_popup.h"

#include <string>

#include "base/base64.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "brave/browser/ui/views/location_bar/brave_location_bar_view.h"
#include "brave/components/brave_rewards/browser/rewards_service.h"
#include "brave/components/brave_shields/common/brave_shield_constants.h"
#include "brave/components/toolbar/constants.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/guest_view/browser/guest_view_base.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/common/referrer.h"
#include "net/base/escape.h"
#include "third_party/blink/public/platform/web_referrer_policy.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/widget/widget.h"

using content::WebContents;

namespace {

constexpr int kPopupPreferredHeight = 800;
constexpr int kPopupPreferredWidth = 1100;

// Referrer.
const char kRewardsHost[] = "rewards";

// URL to open in the popup.
const char kAddFundsUrl[] = "https://uphold-widget.herokuapp.com/index.php";

// Content permission URLs and patterns.
const char kAddFundsOrigin[] = "https://uphold-widget.herokuapp.com/";
const char kFirstParty[] = "https://firstParty";

const char kUpholdPattern[] = "https://[*.]uphold.com/*";
const char kNetverifyPattern[] = "https://[*.]netverify.com/*";
const char kTypekitPattern[] = "https://use.typekit.net/*";

const std::map<std::string, std::string> kCurrencyToNetworkMap {
  {"BTC", "bitcoin"},
  {"BAT", "ethereum"},
  {"ETH", "ethereum"},
  {"LTC", "litecoin"}
};

class PopupContentSettingsBase {
 public:
  PopupContentSettingsBase(Profile* profile);
  virtual ~PopupContentSettingsBase();

 private:
  // Allow settings.
  void Allow();
  void AllowShieldsFingerprinting(HostContentSettingsMap* map);
  void AllowShieldsCookies(HostContentSettingsMap* map);
  void AllowShieldsScripts(HostContentSettingsMap* map);
  void AllowCameraAccess(HostContentSettingsMap* map);
  void AllowAutoplay(HostContentSettingsMap* map);

  // Reset settings to the original values.
  void Reset();
  void ResetShieldsFingerprinting(HostContentSettingsMap* map);
  void ResetShieldsCookies(HostContentSettingsMap* map);
  void ResetShieldsScripts(HostContentSettingsMap* map);
  void ResetCameraAccess(HostContentSettingsMap* map);
  void ResetAutoplay(HostContentSettingsMap* map);

  // Generic settings routines.
  ContentSetting SetContentSetting(HostContentSettingsMap* map,
                                   const char* host,
                                   const char* secondary,
                                   ContentSettingsType type,
                                   ContentSetting setting,
                                   const std::string& resource_identifier);

  void ResetContentSetting(HostContentSettingsMap* map,
                           const char* host,
                           ContentSettingsType type,
                           ContentSetting setting);

  // Profile for which content setting have been altered.
  Profile* profile_;

  // Original content settings.
  ContentSetting referrers_setting_;
  ContentSetting cookies_setting_;
  ContentSetting cookies_setting_fp_;
  ContentSetting fingerprinting_setting_;
  ContentSetting fingerprinting_setting_fp_;
  ContentSetting camera_setting_;
  ContentSetting autoplay_setting_;

  // Set to true if we had to enable javascript.
  bool allowed_scripts;

  DISALLOW_COPY_AND_ASSIGN(PopupContentSettingsBase);
};

PopupContentSettingsBase::PopupContentSettingsBase(Profile* profile)
    : profile_(profile), allowed_scripts(false) {
  Allow();
}

PopupContentSettingsBase::~PopupContentSettingsBase() {
  Reset();
}

void PopupContentSettingsBase::Allow() {
  DCHECK(profile_);
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  DCHECK(map);

  AllowShieldsFingerprinting(map);
  AllowShieldsCookies(map);
  AllowShieldsScripts(map);
  AllowCameraAccess(map);
  AllowAutoplay(map);
}

void PopupContentSettingsBase::AllowShieldsFingerprinting(
    HostContentSettingsMap* map) {
  // Narrower scope first.
  fingerprinting_setting_fp_ = SetContentSetting(
      map, kAddFundsOrigin, kFirstParty, CONTENT_SETTINGS_TYPE_PLUGINS,
      CONTENT_SETTING_ALLOW, brave_shields::kFingerprinting);
  // Wider scope.
  fingerprinting_setting_ = SetContentSetting(
      map, kAddFundsOrigin, nullptr, CONTENT_SETTINGS_TYPE_PLUGINS,
      CONTENT_SETTING_ALLOW, brave_shields::kFingerprinting);
}

void PopupContentSettingsBase::AllowShieldsCookies(
    HostContentSettingsMap* map) {
  referrers_setting_ = SetContentSetting(
      map, kAddFundsOrigin, nullptr, CONTENT_SETTINGS_TYPE_PLUGINS,
      CONTENT_SETTING_ALLOW, brave_shields::kReferrers);
  cookies_setting_fp_ = SetContentSetting(
      map, kAddFundsOrigin, kFirstParty, CONTENT_SETTINGS_TYPE_PLUGINS,
      CONTENT_SETTING_ALLOW, brave_shields::kCookies);
  cookies_setting_ = SetContentSetting(
      map, kAddFundsOrigin, nullptr, CONTENT_SETTINGS_TYPE_PLUGINS,
      CONTENT_SETTING_ALLOW, brave_shields::kCookies);
}

void PopupContentSettingsBase::AllowShieldsScripts(
    HostContentSettingsMap* map) {
  // Check if shields scripts setting is turned to disallow scripts.
  if (map->GetContentSetting(GURL(), GURL(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
                             std::string()) != CONTENT_SETTING_ALLOW) {
    // Allow scripts from our host, uphold.com, and netverify.com.
    const ContentSettingsPattern pattern =
        ContentSettingsPattern::FromString(std::string(kAddFundsOrigin) + "/*");
    map->SetContentSettingCustomScope(
        pattern, ContentSettingsPattern::Wildcard(),
        CONTENT_SETTINGS_TYPE_JAVASCRIPT, std::string(), CONTENT_SETTING_ALLOW);
    map->SetContentSettingCustomScope(
        ContentSettingsPattern::FromString(kUpholdPattern),
        ContentSettingsPattern::Wildcard(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
        std::string(), CONTENT_SETTING_ALLOW);
    map->SetContentSettingCustomScope(
        ContentSettingsPattern::FromString(kNetverifyPattern),
        ContentSettingsPattern::Wildcard(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
        std::string(), CONTENT_SETTING_ALLOW);
    map->SetContentSettingCustomScope(
        ContentSettingsPattern::FromString(kTypekitPattern),
        ContentSettingsPattern::Wildcard(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
        std::string(), CONTENT_SETTING_ALLOW);
    allowed_scripts = true;
  }
}

void PopupContentSettingsBase::AllowCameraAccess(HostContentSettingsMap* map) {
  camera_setting_ = SetContentSetting(map, kAddFundsOrigin, nullptr,
                                      CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA,
                                      CONTENT_SETTING_ALLOW, std::string());
}

void PopupContentSettingsBase::AllowAutoplay(HostContentSettingsMap* map) {
  autoplay_setting_ = SetContentSetting(map, kAddFundsOrigin, nullptr,
                                        CONTENT_SETTINGS_TYPE_AUTOPLAY,
                                        CONTENT_SETTING_ALLOW, std::string());
}

void PopupContentSettingsBase::Reset() {
  DCHECK(profile_);
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  DCHECK(map);

  ResetShieldsFingerprinting(map);
  ResetShieldsCookies(map);
  ResetShieldsScripts(map);
  ResetCameraAccess(map);
  ResetAutoplay(map);
}

void PopupContentSettingsBase::ResetShieldsFingerprinting(
    HostContentSettingsMap* map) {
  // Wide scope first.
  SetContentSetting(map, kAddFundsOrigin, nullptr,
                    CONTENT_SETTINGS_TYPE_PLUGINS, fingerprinting_setting_,
                    brave_shields::kFingerprinting);
  // Then narrow scope.
  SetContentSetting(map, kAddFundsOrigin, kFirstParty,
                    CONTENT_SETTINGS_TYPE_PLUGINS, fingerprinting_setting_fp_,
                    brave_shields::kFingerprinting);
}

void PopupContentSettingsBase::ResetShieldsCookies(
    HostContentSettingsMap* map) {
  SetContentSetting(map, kAddFundsOrigin, nullptr,
                    CONTENT_SETTINGS_TYPE_PLUGINS, referrers_setting_,
                    brave_shields::kReferrers);
  SetContentSetting(map, kAddFundsOrigin, nullptr,
                    CONTENT_SETTINGS_TYPE_PLUGINS, cookies_setting_,
                    brave_shields::kCookies);
  SetContentSetting(map, kAddFundsOrigin, kFirstParty,
                    CONTENT_SETTINGS_TYPE_PLUGINS, cookies_setting_fp_,
                    brave_shields::kCookies);
}

void PopupContentSettingsBase::ResetShieldsScripts(
    HostContentSettingsMap* map) {
  if (allowed_scripts) {
    // Delete entries for our host, uphold.com, and netverify.com.
    const ContentSettingsPattern pattern =
        ContentSettingsPattern::FromString(std::string(kAddFundsOrigin) + "/*");
    map->SetContentSettingCustomScope(pattern,
                                      ContentSettingsPattern::Wildcard(),
                                      CONTENT_SETTINGS_TYPE_JAVASCRIPT,
                                      std::string(), CONTENT_SETTING_DEFAULT);
    map->SetContentSettingCustomScope(
        ContentSettingsPattern::FromString(kUpholdPattern),
        ContentSettingsPattern::Wildcard(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
        std::string(), CONTENT_SETTING_DEFAULT);
    map->SetContentSettingCustomScope(
        ContentSettingsPattern::FromString(kNetverifyPattern),
        ContentSettingsPattern::Wildcard(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
        std::string(), CONTENT_SETTING_DEFAULT);
    map->SetContentSettingCustomScope(
        ContentSettingsPattern::FromString(kTypekitPattern),
        ContentSettingsPattern::Wildcard(), CONTENT_SETTINGS_TYPE_JAVASCRIPT,
        std::string(), CONTENT_SETTING_DEFAULT);
    allowed_scripts = false;
  }
}

void PopupContentSettingsBase::ResetCameraAccess(HostContentSettingsMap* map) {
  ResetContentSetting(map, kAddFundsOrigin,
                      CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA,
                      camera_setting_);
}

void PopupContentSettingsBase::ResetAutoplay(HostContentSettingsMap* map) {
  ResetContentSetting(map, kAddFundsOrigin, CONTENT_SETTINGS_TYPE_AUTOPLAY,
                      autoplay_setting_);
}

ContentSetting PopupContentSettingsBase::SetContentSetting(
    HostContentSettingsMap* map,
    const char* host,
    const char* secondary,
    ContentSettingsType type,
    ContentSetting setting,
    const std::string& resource_identifier) {
  DCHECK(map);
  DCHECK(host);

  const GURL gurl(host);
  GURL gurl_secondary;
  if (secondary) {
    gurl_secondary = GURL(secondary);
  }

  // Get the current setting.
  ContentSetting current_setting =
      map->GetContentSetting(gurl, gurl_secondary, type, resource_identifier);

  // Check if the setting is already want we want it to be.
  if (current_setting != setting) {
    // For PLUGINS type, construct patterns and use custom scope.
    if (type == CONTENT_SETTINGS_TYPE_PLUGINS) {
      const ContentSettingsPattern pattern =
          ContentSettingsPattern::FromString(std::string(host) + "/*");
      ContentSettingsPattern pattern_secondary =
          ContentSettingsPattern::Wildcard();
      if (secondary)
        pattern_secondary =
            ContentSettingsPattern::FromString(std::string(secondary) + "/*");

      map->SetContentSettingCustomScope(pattern, pattern_secondary, type,
                                        resource_identifier, setting);
    } else {
      // For other types use default scope.
      ContentSetting default_setting =
          map->GetDefaultContentSetting(type, nullptr);
      if (current_setting == default_setting)
        current_setting = CONTENT_SETTING_DEFAULT;
      map->SetContentSettingDefaultScope(gurl, gurl_secondary, type,
                                         resource_identifier, setting);
    }
  }

  return current_setting;
}

void PopupContentSettingsBase::ResetContentSetting(HostContentSettingsMap* map,
                                                   const char* host,
                                                   ContentSettingsType type,
                                                   ContentSetting setting) {
  DCHECK(map);
  DCHECK(host);
  DCHECK(type != CONTENT_SETTINGS_TYPE_PLUGINS);

  const GURL gurl(host);
  if (setting == CONTENT_SETTING_DEFAULT ||
      setting != map->GetContentSetting(gurl, GURL(), type, std::string())) {
    map->SetContentSettingDefaultScope(gurl, GURL(), type, std::string(),
                                       setting);
  }
}

}  // namespace

namespace brave_rewards {

// Pass-through to PopupContentSettingsBase which is in an anonymous namespace.
class AddFundsPopupContentSettings : public PopupContentSettingsBase {
 public:
  AddFundsPopupContentSettings(Profile* profile)
      : PopupContentSettingsBase(profile) {}
  DISALLOW_COPY_AND_ASSIGN(AddFundsPopupContentSettings);
};

AddFundsPopup::AddFundsPopup()
    : add_funds_popup_(nullptr),
      popup_content_settings_(nullptr),
      rewards_service_(nullptr) {}

AddFundsPopup::~AddFundsPopup() {
  ClosePopup();
}

// Show existing or open a new popup.
void AddFundsPopup::ShowPopup(content::WebContents* initiator,
                              RewardsService* rewards_service) {
  add_funds_popup_ ? Focus() : OpenPopup(initiator, rewards_service);
}

void AddFundsPopup::OpenPopup(content::WebContents* initiator,
                              RewardsService* rewards_service) {
  DCHECK(!add_funds_popup_);
  DCHECK(initiator);
  DCHECK(rewards_service);
  if (!initiator || !rewards_service)
    return;

  const std::map<std::string, std::string> addresses =
      rewards_service->GetAddresses();
  if (addresses.empty())
    return;

  content::WebContentsDelegate* wc_delegate = initiator->GetDelegate();
  if (!wc_delegate)
    return;

  const GURL gurl(kAddFundsUrl);
  const GURL referrer_gurl(std::string(brave_toolbar::kInternalUIScheme) +
                           kRewardsHost);
  const content::Referrer referrer(referrer_gurl,
                                   blink::kWebReferrerPolicyAlways);
  content::OpenURLParams params(gurl, referrer,
                                WindowOpenDisposition::NEW_POPUP,
                                ui::PAGE_TRANSITION_LINK, true);

  // Supply addresses via post data. The data is currently in the query string
  // format (application/x-www-form-urlencoded):
  // addresses=UrlEscapedBase64EncodedStringifiedJSON.
  params.uses_post = true;
  const std::string data = ToQueryString(GetAddressesAsJSON(addresses));
  params.post_data =
      network::ResourceRequestBody::CreateFromBytes(data.data(), data.size());
  params.extra_headers =
      std::string("Content-Type: application/x-www-form-urlencoded\r\n") +
      "Content-Length: " + std::to_string(data.size()) + "\r\n\r\n";

  // Let popup content bypass shields, use camera and autoplay.
  std::unique_ptr<AddFundsPopupContentSettings> popup_content_settings =
      EnsureContentPermissions(initiator);

  // Open the popup.
  add_funds_popup_ = wc_delegate->OpenURLFromTab(initiator, params);
  DCHECK(add_funds_popup_);
  if (!add_funds_popup_)
    return;

  // We need to know when the popup closes.
  views::Widget* topLevelWidget = views::Widget::GetTopLevelWidgetForNativeView(
      add_funds_popup_->GetNativeView());
  if (!topLevelWidget) {
    // If we can't add an observer won't be able to reset when the popup closes
    // and generally this is not a good sign, so don't bother with the popup.
    ClosePopup();
    return;
  }
  topLevelWidget->AddObserver(this);

  // Keep track of the settings we changed.
  popup_content_settings_ = std::move(popup_content_settings);

  // Reposition/resize the new popup and hide actions.
  gfx::Rect popup_bounds = CalculatePopupWindowBounds(initiator);
  topLevelWidget->SetBounds(popup_bounds);
  HideBraveActions();
  Focus();

  // Stash rewards service pointer so that we can call it to update wallet
  // info when the popup is closed by the user.
  rewards_service_ = rewards_service;
}

void AddFundsPopup::ClosePopup() {
  if (!add_funds_popup_)
    return;

  views::Widget* widget = views::Widget::GetTopLevelWidgetForNativeView(
      add_funds_popup_->GetNativeView());
  if (widget)
    widget->RemoveObserver(this);

  add_funds_popup_->ClosePage();
  add_funds_popup_ = nullptr;
}

void AddFundsPopup::Focus() {
  DCHECK(add_funds_popup_);
  content::WebContentsDelegate* popup_delegate =
      add_funds_popup_->GetDelegate();
  if (popup_delegate)
    popup_delegate->ActivateContents(add_funds_popup_);
  add_funds_popup_->Focus();
}

// content::WidgetObserver implementation.
void AddFundsPopup::OnWidgetClosing(views::Widget* widget) {
  widget->RemoveObserver(this);
  popup_content_settings_.reset(nullptr);
  add_funds_popup_ = nullptr;
  if (rewards_service_)
    rewards_service_->FetchWalletProperties();
}

std::string AddFundsPopup::GetAddressesAsJSON(
    const std::map<std::string, std::string>& addresses) {
  // Create a dictionary of addresses for serialization.
  auto addresses_dictionary = std::make_unique<base::DictionaryValue>();
  for (const auto& pair : addresses) {
    auto address = std::make_unique<base::DictionaryValue>();
    address->SetString("address", pair.second);
    address->SetString("currency", pair.first);
    DCHECK(kCurrencyToNetworkMap.count(pair.first));
    address->SetString("network", kCurrencyToNetworkMap.count(pair.first)
                                      ? kCurrencyToNetworkMap.at(pair.first)
                                      : "");
    addresses_dictionary->SetDictionary(pair.first, std::move(address));
  }

  std::string data;
  base::JSONWriter::Write(*addresses_dictionary, &data);
  return data;
}

std::string AddFundsPopup::ToQueryString(const std::string& data) {
  std::string query_string_value;
  base::Base64Encode(data, &query_string_value);
  return ("addresses=" + net::EscapeUrlEncodedData(query_string_value, false));
}

gfx::Rect AddFundsPopup::CalculatePopupWindowBounds(WebContents* initiator) {
  // Get bounds of the initiator content to see if they would fit the
  // preferred size of our popup.
  WebContents* outermost_web_contents =
      guest_view::GuestViewBase::GetTopLevelWebContents(initiator);
  gfx::Rect initiator_bounds = outermost_web_contents->GetContainerBounds();

  gfx::Point center = initiator_bounds.CenterPoint();
  gfx::Rect popup_bounds(center.x() - kPopupPreferredWidth / 2,
                         center.y() - kPopupPreferredHeight / 2,
                         kPopupPreferredWidth, kPopupPreferredHeight);
  // Popup fits within the initiator, return the bounds no matter where the
  // initiator is on the screen.
  if (initiator_bounds.Contains(popup_bounds))
    return popup_bounds;

  // Move the popup to the center of the screen if it ended up off screen.
  // If the initiator is split between multiple displays this will show the
  // popup on the display that contains the largest chunk of the initiator
  // window. If the popup is too big for the screen, shrink it to fit.
  display::Display display =
      display::Screen::GetScreen()->GetDisplayNearestView(
          outermost_web_contents->GetNativeView());
  if (!display.bounds().IsEmpty() && !display.bounds().Contains(popup_bounds)) {
    popup_bounds = display.bounds();
    popup_bounds.ClampToCenteredSize(
        gfx::Size(kPopupPreferredWidth, kPopupPreferredHeight));
  }

  return popup_bounds;
}

std::unique_ptr<AddFundsPopupContentSettings>
AddFundsPopup::EnsureContentPermissions(content::WebContents* initiator) {
  DCHECK(initiator);
  // Get contents settings map for the current profile.
  Profile* profile =
      Profile::FromBrowserContext(initiator->GetBrowserContext());
  DCHECK(profile && !profile->IsOffTheRecord());
  return std::make_unique<AddFundsPopupContentSettings>(profile);
}

void AddFundsPopup::HideBraveActions() {
  if (!add_funds_popup_)
    return;

  Browser* browser = chrome::FindBrowserWithWebContents(add_funds_popup_);
  if (!browser)
    return;

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view)
    return;

  LocationBarView* location_bar_view = browser_view->GetLocationBarView();
  if (!location_bar_view)
    return;

  BraveLocationBarView* brave_location_bar_view =
      location_bar_view->GetBraveLocationBarView();
  if (!brave_location_bar_view)
    return;

  brave_location_bar_view->HideBraveActionsContainer();
}

}  // namespace brave_rewards
