/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_ads/browser/ads_tab_helper.h"

#include "brave/components/brave_ads/browser/ads_service.h"
#include "brave/components/brave_ads/browser/ads_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_tab_helper.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"

using content::ResourceType;

namespace brave_ads {

AdsTabHelper::AdsTabHelper(content::WebContents* web_contents)
    : WebContentsObserver(web_contents),
      tab_id_(SessionTabHelper::IdForTab(web_contents)),
      ads_service_(nullptr) {
  if (!tab_id_.is_valid())
    return;

  BrowserList::AddObserver(this);
  Profile* profile = Profile::FromBrowserContext(
      web_contents->GetBrowserContext());
  ads_service_ = AdsServiceFactory::GetForProfile(profile);
}

AdsTabHelper::~AdsTabHelper() {
  BrowserList::RemoveObserver(this);
}

// TODO(bridiver) ClassifyPage

void AdsTabHelper::DidFinishLoad(
    content::RenderFrameHost* render_frame_host,
    const GURL& validated_url) {
  if (render_frame_host->GetParent())
    return;

  TabUpdated();
}

void AdsTabHelper::DidAttachInterstitialPage() {
  TabUpdated();
}

void AdsTabHelper::TabUpdated() {
  if (!ads_service_)
    return;

  ads_service_->TabUpdated(
      tab_id_,
      web_contents()->GetURL(),
      is_active_ && is_browser_active_);
}

void AdsTabHelper::MediaStartedPlaying(const MediaPlayerInfo& video_type,
                         const MediaPlayerId& id) {
  if (ads_service_)
    ads_service_->OnMediaStart(tab_id_);
}

void AdsTabHelper::MediaStoppedPlaying(
    const MediaPlayerInfo& video_type,
    const MediaPlayerId& id,
    WebContentsObserver::MediaStoppedReason reason) {
  if (ads_service_)
    ads_service_->OnMediaStop(tab_id_);
}

void AdsTabHelper::OnVisibilityChanged(content::Visibility visibility) {
  bool old_active = is_active_;
  if (visibility == content::Visibility::HIDDEN) {
    is_active_ = false;
  } else if (visibility == content::Visibility::OCCLUDED) {
    is_active_ = false;
  } else if (visibility == content::Visibility::VISIBLE) {
    is_active_ = true;
  }

  if (old_active != is_active_)
    TabUpdated();
}

void AdsTabHelper::WebContentsDestroyed() {
  LOG(ERROR) << "1 " << ads_service_;
  if (ads_service_) {
    LOG(ERROR) << "2";
    ads_service_->TabClosed(tab_id_);
    LOG(ERROR) << "3";
  }
}

void AdsTabHelper::OnBrowserSetLastActive(Browser* browser) {
  bool old_active = is_browser_active_;
  if (browser->tab_strip_model()->GetIndexOfWebContents(web_contents()) !=
      TabStripModel::kNoTab) {
    is_browser_active_ = true;
  }

  if (old_active != is_browser_active_)
    TabUpdated();
}

void AdsTabHelper::OnBrowserNoLongerActive(Browser* browser) {
  bool old_active = is_browser_active_;
  if (browser->tab_strip_model()->GetIndexOfWebContents(web_contents()) !=
      TabStripModel::kNoTab) {
    is_browser_active_ = false;
  }

  if (old_active != is_browser_active_)
    TabUpdated();
}

}  // namespace brave_ads
