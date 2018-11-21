/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_ads/browser/ads_service_impl.h"

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/sequenced_task_runner.h"
#include "base/task_runner_util.h"
#include "base/task/post_task.h"
#include "bat/ads/ads.h"
#include "bat/ads/notification_info.h"
#include "bat/ads/resources/grit/bat_ads_resources.h"
#include "brave/components/brave_ads/browser/ad_notification.h"
#include "brave/components/brave_ads/browser/bundle_state_database.h"
#include "brave/components/brave_ads/common/pref_names.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_impl.h"
#include "chrome/browser/notifications/notification_handler.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/chrome_constants.h"
#include "components/prefs/pref_service.h"
#include "components/wifi/wifi_service.h"
#include "content/public/browser/browser_thread.h"
#include "net/url_request/url_fetcher.h"
#include "third_party/dom_distiller_js/dom_distiller.pb.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/message_center/public/cpp/notification.h"

#if defined(OS_ANDROID)
#include "net/android/network_library.h"
#endif

namespace brave_ads {

class AdsNotificationHandler : public NotificationHandler {
 public:
  AdsNotificationHandler(AdsServiceImpl* ads_service) :
      ads_service_(ads_service->AsWeakPtr()) {}

  ~AdsNotificationHandler() override {}

  // NotificationHandler implementation.
  void OnShow(Profile* profile,
              const std::string& notification_id) override {
    if (ads_service_)
      ads_service_->OnShow(profile, notification_id);
  }

  void OnClose(Profile* profile,
               const GURL& origin,
               const std::string& notification_id,
               bool by_user,
               base::OnceClosure completed_closure) override {
    if (!ads_service_) {
      std::move(completed_closure).Run();
      return;
    }

    ads_service_->OnClose(
        profile, origin, notification_id, by_user,
        std::move(completed_closure));
  }

  void DisableNotifications(Profile* profile,
                            const GURL& origin) override {}


  void OpenSettings(Profile* profile, const GURL& origin) override {
    if (ads_service_)
      ads_service_->OpenSettings(profile, origin);
  }

 private:

  base::WeakPtr<AdsServiceImpl> ads_service_;

  DISALLOW_COPY_AND_ASSIGN(AdsNotificationHandler);
};

namespace {

static std::map<std::string, int> g_schema_resource_ids = {
  {"catalog", IDR_ADS_CATALOG_SCHEMA},
  {"bundle", IDR_ADS_BUNDLE_SCHEMA},
};

int GetSchemaResourceId(const std::string& name) {
  if (g_schema_resource_ids.find(name) != g_schema_resource_ids.end())
    return g_schema_resource_ids[name];

  NOTREACHED();
  return 0;
}

static std::map<std::string, int> g_user_model_resource_ids = {
  {"de", IDR_ADS_USER_MODEL_DE},
  {"fr", IDR_ADS_USER_MODEL_FR},
  {"en", IDR_ADS_USER_MODEL_EN},
};

int GetUserModelResourceId(const std::string& locale) {
  if (g_user_model_resource_ids.find(locale) != g_user_model_resource_ids.end())
    return g_user_model_resource_ids[locale];

  NOTREACHED();
  return 0;
}

net::URLFetcher::RequestType URLMethodToRequestType(
    ads::URLRequestMethod method) {
  switch(method) {
    case ads::URLRequestMethod::GET:
      return net::URLFetcher::RequestType::GET;
    case ads::URLRequestMethod::POST:
      return net::URLFetcher::RequestType::POST;
    case ads::URLRequestMethod::PUT:
      return net::URLFetcher::RequestType::PUT;
    default:
      NOTREACHED();
      return net::URLFetcher::RequestType::GET;
  }
}

void PostWriteCallback(
    const base::Callback<void(bool success)>& callback,
    scoped_refptr<base::SequencedTaskRunner> reply_task_runner,
    bool success) {
  // We can't run |callback| on the current thread. Bounce back to
  // the |reply_task_runner| which is the correct sequenced thread.
  reply_task_runner->PostTask(FROM_HERE,
                              base::Bind(callback, success));
}

std::string LoadOnFileTaskRunner(
    const base::FilePath& path) {
  std::string data;
  bool success = base::ReadFileToString(path, &data);

  // Make sure the file isn't empty.
  if (!success || data.empty()) {
    LOG(ERROR) << "Failed to read file: " << path.MaybeAsASCII();
    return std::string();
  }
  return data;
}

std::vector<ads::AdInfo> GetAdsForCategoryOnFileTaskRunner(
    const std::string category,
    BundleStateDatabase* backend) {
  std::vector<ads::AdInfo> ads;
  if (!backend)
    return ads;

  backend->GetAdsForCategory(category, ads);

  return ads;
}

bool ResetOnFileTaskRunner(
    const base::FilePath& path) {
  return base::DeleteFile(path, false);
}

bool SaveBundleStateOnFileTaskRunner(
    std::unique_ptr<ads::BundleState> bundle_state,
    BundleStateDatabase* backend) {
  if (backend && backend->SaveBundleState(*bundle_state))
    return true;

  return false;
}

}

AdsServiceImpl::AdsServiceImpl(Profile* profile) :
    profile_(profile),
    file_task_runner_(base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
    base_path_(profile_->GetPath().AppendASCII("ads_service")),
    next_timer_id_(0),
    bundle_state_backend_(
        new BundleStateDatabase(base_path_.AppendASCII("bundle_state"))),
    display_service_(NotificationDisplayService::GetForProfile(profile_)),
    enabled_(false),
    last_idle_state_(ui::IdleState::IDLE_STATE_ACTIVE),
    is_foreground_(!!chrome::FindBrowserWithActiveWindow()) {
  DCHECK(!profile_->IsOffTheRecord());

  profile_pref_change_registrar_.Init(profile_->GetPrefs());
  profile_pref_change_registrar_.Add(
      prefs::kBraveAdsEnabled,
      base::Bind(&AdsServiceImpl::OnPrefsChanged,
                 base::Unretained(this)));
  profile_pref_change_registrar_.Add(
      prefs::kBraveAdsIdleThreshold,
      base::Bind(&AdsServiceImpl::OnPrefsChanged,
                 base::Unretained(this)));

  auto* display_service_impl =
      static_cast<NotificationDisplayServiceImpl*>(display_service_);

  display_service_impl->AddNotificationHandler(
      NotificationHandler::Type::BRAVE_ADS,
      std::make_unique<AdsNotificationHandler>(this));

  if (is_enabled())
    Start();
}

AdsServiceImpl::~AdsServiceImpl() {
  file_task_runner_->DeleteSoon(FROM_HERE, bundle_state_backend_.release());
}

void AdsServiceImpl::Start() {
  DCHECK(is_enabled());
  enabled_ = true;
  ads_.reset(ads::Ads::CreateInstance(this));
  ResetTimer();
}

void AdsServiceImpl::Stop() {
  enabled_ = false;
  Shutdown();
}

void AdsServiceImpl::ResetTimer() {
  idle_poll_timer_.Stop();
  idle_poll_timer_.Start(FROM_HERE,
                         base::TimeDelta::FromSeconds(GetIdleThreshold()), this,
                         &AdsServiceImpl::CheckIdleState);
}

void AdsServiceImpl::CheckIdleState() {
  ui::CalculateIdleState(GetIdleThreshold(),
      base::BindRepeating(&AdsServiceImpl::OnIdleState,
          base::Unretained(this)));
}

void AdsServiceImpl::OnIdleState(ui::IdleState idle_state) {
  if (!ads_ || idle_state == last_idle_state_)
    return;

  if (idle_state == ui::IdleState::IDLE_STATE_ACTIVE)
    ads_->OnUnIdle();
  else
    ads_->OnIdle();

  last_idle_state_ = idle_state;
}

void AdsServiceImpl::Shutdown() {
  fetchers_.clear();
  idle_poll_timer_.Stop();

  if (ads_) {
    ads_->SaveCachedInfo();
    ads_.reset();
  }
  for (NotificationInfoMap::iterator it = notification_ids_.begin();
      it != notification_ids_.end(); ++it) {
    const std::string notification_id = it->first;
    display_service_->Close(NotificationHandler::Type::BRAVE_ADS,
                            notification_id);
  }
  notification_ids_.clear();
}

void AdsServiceImpl::OnPrefsChanged(const std::string& pref) {
  if (pref == prefs::kBraveAdsEnabled) {
    if (is_enabled() && !enabled_) {
      Start();
    } else if (!is_enabled() && enabled_) {
      Stop();
    }
  } else if (pref == prefs::kBraveAdsIdleThreshold) {
    ResetTimer();
  }
}

bool AdsServiceImpl::is_enabled() const {
  return profile_->GetPrefs()->GetBoolean(prefs::kBraveAdsEnabled);
}

bool AdsServiceImpl::IsAdsEnabled() const {
  return is_enabled();
}

void AdsServiceImpl::TabUpdated(SessionID tab_id,
                                const GURL& url,
                                const bool is_active) {
  if (!ads_)
    return;

  ads_->TabUpdated(tab_id.id(),
                   url.spec(),
                   is_active,
                   profile_->IsOffTheRecord());

  if (is_foreground_ && !chrome::FindBrowserWithActiveWindow()) {
    ads_->OnBackground();
  } else if (!is_foreground_) {
    is_foreground_ = true;
    ads_->OnForeground();
  }
}

void AdsServiceImpl::TabClosed(SessionID tab_id) {
  if (!ads_)
    return

  ads_->TabClosed(tab_id.id());
}

void AdsServiceImpl::ClassifyPage(const std::string& url,
                                  const std::string& page) {
  if (!ads_)
    return

  ads_->ClassifyPage(url, page);
}

int AdsServiceImpl::GetIdleThreshold() {
  return profile_->GetPrefs()->GetInteger(prefs::kBraveAdsIdleThreshold);
}

void AdsServiceImpl::SetIdleThreshold(const int threshold) {
  profile_->GetPrefs()->SetInteger(prefs::kBraveAdsIdleThreshold, threshold);
}

bool AdsServiceImpl::IsNotificationsAvailable() const {
  #if BUILDFLAG(ENABLE_NATIVE_NOTIFICATIONS)
  return true;
#else
  return false;
#endif
}

bool AdsServiceImpl::IsNotificationsExpired() const {
  // TODO(bridiver) - is this still relevant?
  return false;
}

void AdsServiceImpl::GetUserModelForLocale(const std::string& locale,
                                           ads::OnLoadCallback callback) const {
  base::StringPiece user_model_raw =
      ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
          GetUserModelResourceId(locale));

  std::string user_model;
  user_model_raw.CopyToString(&user_model);
  callback(ads::Result::SUCCESS, user_model);
}

void AdsServiceImpl::OnURLsDeleted(history::HistoryService* history_service,
                   const history::DeletionInfo& deletion_info) {
  if (!ads_)
    return;

  ads_->RemoveAllHistory();
}

void AdsServiceImpl::OnMediaStart(SessionID tab_id) {
  if (!ads_)
    return;

  ads_->OnMediaPlaying(tab_id.id());
}

void AdsServiceImpl::OnMediaStop(SessionID tab_id) {
  if (!ads_)
    return;

  ads_->OnMediaStopped(tab_id.id());
}

uint64_t AdsServiceImpl::GetAdsPerHour() const {
  return profile_->GetPrefs()->GetUint64(prefs::kBraveAdsPerHour);
}

uint64_t AdsServiceImpl::GetAdsPerDay() const {
  return profile_->GetPrefs()->GetUint64(prefs::kBraveAdsPerDay);
}

void AdsServiceImpl::ShowNotification(
    std::unique_ptr<ads::NotificationInfo> info) {
  std::string notification_id;
  auto notification =
      CreateAdNotification(*info, &notification_id);

  notification_ids_[notification_id] = std::move(info);

  display_service_->Display(NotificationHandler::Type::BRAVE_ADS,
                            *notification);
}

void AdsServiceImpl::Save(const std::string& name,
                          const std::string& value,
                          ads::OnSaveCallback callback) {
  base::ImportantFileWriter writer(
      base_path_.AppendASCII(name), file_task_runner_);

  writer.RegisterOnNextWriteCallbacks(
      base::Closure(),
      base::Bind(
        &PostWriteCallback,
        base::Bind(&AdsServiceImpl::OnSaved, AsWeakPtr(),
            std::move(callback)),
        base::SequencedTaskRunnerHandle::Get()));

  writer.WriteNow(std::make_unique<std::string>(value));
}

void AdsServiceImpl::Load(const std::string& name,
                          ads::OnLoadCallback callback) {
  base::PostTaskAndReplyWithResult(file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&LoadOnFileTaskRunner, base_path_.AppendASCII(name)),
      base::BindOnce(&AdsServiceImpl::OnLoaded,
                     AsWeakPtr(),
                     std::move(callback)));
}

const std::string AdsServiceImpl::LoadSchema(const std::string& name) {
  base::StringPiece schema_raw =
      ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
          GetSchemaResourceId(name));

  std::string schema;
  schema_raw.CopyToString(&schema);
  return schema;
}

void AdsServiceImpl::SaveBundleState(
    std::unique_ptr<ads::BundleState> bundle_state,
    ads::OnSaveCallback callback) {
  base::PostTaskAndReplyWithResult(file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&SaveBundleStateOnFileTaskRunner,
                    base::Passed(std::move(bundle_state)),
                    bundle_state_backend_.get()),
      base::BindOnce(&AdsServiceImpl::OnSaveBundleState,
                     AsWeakPtr(),
                     callback));
}

void AdsServiceImpl::OnSaveBundleState(const ads::OnSaveCallback& callback,
                                       bool success) {
  callback(success ? ads::Result::SUCCESS : ads::Result::FAILED);
}

void AdsServiceImpl::OnLoaded(
    const ads::OnLoadCallback& callback,
    const std::string& value) {
  if (value.empty())
    callback(ads::Result::FAILED, value);
  else
    callback(ads::Result::SUCCESS, value);
}

void AdsServiceImpl::OnSaved(
    const ads::OnSaveCallback& callback,
    bool success) {
  callback(success ? ads::Result::SUCCESS : ads::Result::FAILED);
}

void AdsServiceImpl::Reset(const std::string& name,
                           ads::OnResetCallback callback) {
  base::PostTaskAndReplyWithResult(file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ResetOnFileTaskRunner, base_path_.AppendASCII(name)),
      base::BindOnce(&AdsServiceImpl::OnReset,
                     AsWeakPtr(),
                     std::move(callback)));
}

void AdsServiceImpl::OnReset(const ads::OnResetCallback& callback,
                             bool success) {
  callback(success ? ads::Result::SUCCESS : ads::Result::FAILED);
}

void AdsServiceImpl::GetAdsForCategory(
      const std::string& category,
      ads::OnGetAdsForCategoryCallback callback) {
  base::PostTaskAndReplyWithResult(file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&GetAdsForCategoryOnFileTaskRunner,
                    category,
                    bundle_state_backend_.get()),
      base::BindOnce(&AdsServiceImpl::OnGetAdsForCategory,
                     AsWeakPtr(),
                     std::move(callback),
                     category));
}

void AdsServiceImpl::OnGetAdsForCategory(
    const ads::OnGetAdsForCategoryCallback& callback,
    const std::string& category,
    const std::vector<ads::AdInfo>& ads) {
  callback(ads.empty() ? ads::Result::FAILED : ads::Result::SUCCESS,
      category,
      ads);
}

void AdsServiceImpl::GetAdSampleBundle(
    ads::OnGetAdSampleBundleCallback callback) {
  base::StringPiece sample_bundle_raw =
      ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
          IDR_ADS_SAMPLE_BUNDLE);

  std::string sample_bundle;
  sample_bundle_raw.CopyToString(&sample_bundle);
  callback(ads::Result::SUCCESS, sample_bundle);
}

void AdsServiceImpl::OnShow(Profile* profile,
                            const std::string& notification_id) {
  if (!ads_ ||
      notification_ids_.find(notification_id) == notification_ids_.end())
    return;

  ads_->GenerateAdReportingNotificationShownEvent(
      *notification_ids_[notification_id]);
}

void AdsServiceImpl::OnClose(Profile* profile,
                             const GURL& origin,
                             const std::string& notification_id,
                             bool by_user,
                             base::OnceClosure completed_closure) {
  if (ads_ &&
      notification_ids_.find(notification_id) != notification_ids_.end()) {
    auto notification_info = base::WrapUnique(
        notification_ids_[notification_id].release());
    notification_ids_.erase(notification_id);

    auto result_type = by_user
        ? ads::NotificationResultInfoResultType::DISMISSED
        : ads::NotificationResultInfoResultType::TIMEOUT;
    ads_->GenerateAdReportingNotificationResultEvent(
        *notification_info, result_type);
  }

  std::move(completed_closure).Run();
}

void AdsServiceImpl::OpenSettings(Profile* profile,
                                  const GURL& origin) {
  DCHECK(origin.has_query());
  auto notification_id = origin.query();

  if (!ads_ ||
      notification_ids_.find(notification_id) == notification_ids_.end())
    return;

  auto notification_info = base::WrapUnique(
      notification_ids_[notification_id].release());
  notification_ids_.erase(notification_id);

  ads_->GenerateAdReportingNotificationResultEvent(
      *notification_info, ads::NotificationResultInfoResultType::CLICKED);

  GURL url(notification_info->url);

  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  NavigateParams nav_params(browser, url, ui::PAGE_TRANSITION_LINK);
  nav_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  // TODO(bridiver) - what to put here?
  // nav_params.referrer = GURL("https://brave.com");
  nav_params.window_action = NavigateParams::SHOW_WINDOW;
  Navigate(&nav_params);
}

void AdsServiceImpl::GetClientInfo(ads::ClientInfo* client_info) const {
  // TODO(bridiver) - these eventually get used in a catalog request
  // and seem like potential privacy issues

  // this doesn't seem necessary
  client_info->application_version = "";
  // client_info.application_version = chrome::kChromeVersion;

  // this doesn't seem necessary
  client_info->platform = "";
  // client_info.platform = base::OperatingSystemName();

  // this is definitely a privacy issue
  client_info->platform_version = "";
}

const std::string AdsServiceImpl::GenerateUUID() const {
  return base::GenerateGUID();
}

const std::string AdsServiceImpl::GetSSID() const {
    std::string ssid;
#if defined(OS_WIN) || defined(OS_MACOSX)
  std::unique_ptr<wifi::WiFiService> wifi_service(wifi::WiFiService::Create());
  wifi_service->Initialize(nullptr);
  std::string error;
  wifi_service->GetConnectedNetworkSSID(&ssid, &error);
  if (!error.empty())
    return std::string();
#elif defined(OS_LINUX)
  ssid = net::GetWifiSSID();
#elif defined(OS_ANDROID)
  ssid = net::android::GetWifiSSID();
#endif
  // TODO: Handle non UTF8 SSIDs.
  if (!base::IsStringUTF8(ssid))
    return std::string();
  return ssid;
}

const std::vector<std::string> AdsServiceImpl::GetLocales() const {
  std::vector<std::string> locales;

  for (std::map<std::string, int>::iterator it =
          g_user_model_resource_ids.begin();
        it != g_user_model_resource_ids.end();
        ++it) {
    locales.push_back(it->first);
  }

  return locales;
}

const std::string AdsServiceImpl::GetAdsLocale() const {
  return g_browser_process->GetApplicationLocale();
}

void AdsServiceImpl::URLRequest(
      const std::string& url,
      const std::vector<std::string>& headers,
      const std::string& content,
      const std::string& content_type,
      ads::URLRequestMethod method,
      ads::URLRequestCallback callback) {
  net::URLFetcher::RequestType request_type = URLMethodToRequestType(method);

  net::URLFetcher* fetcher = net::URLFetcher::Create(
      GURL(url), request_type, this).release();
  fetcher->SetRequestContext(g_browser_process->system_request_context());

  for (size_t i = 0; i < headers.size(); i++)
    fetcher->AddExtraRequestHeader(headers[i]);

  if (!content.empty())
    fetcher->SetUploadData(content_type, content);

  fetchers_[fetcher] = callback;
}

void AdsServiceImpl::OnURLFetchComplete(
    const net::URLFetcher* source) {
  if (fetchers_.find(source) == fetchers_.end())
    return;

  auto callback = fetchers_[source];
  fetchers_.erase(source);
  int response_code = source->GetResponseCode();
  std::string body;
  std::map<std::string, std::string> headers;
  scoped_refptr<net::HttpResponseHeaders> headersList = source->GetResponseHeaders();

  if (headersList) {
    size_t iter = 0;
    std::string key;
    std::string value;
    while (headersList->EnumerateHeaderLines(&iter, &key, &value)) {
      key = base::ToLowerASCII(key);
      headers[key] = value;
    }
  }

  if (response_code != net::URLFetcher::ResponseCode::RESPONSE_CODE_INVALID &&
      source->GetStatus().is_success()) {
    source->GetResponseAsString(&body);
  }

  callback(response_code, body, headers);
}

bool AdsServiceImpl::GetUrlComponents(
      const std::string& url,
      ads::UrlComponents* components) const {
  GURL gurl(url);

  if (!gurl.is_valid())
    return false;

  components->url = gurl.spec();
  if (gurl.has_scheme())
    components->scheme = gurl.scheme();

  if (gurl.has_username())
    components->user = gurl.username();

  if (gurl.has_host())
    components->hostname = gurl.host();

  if (gurl.has_port())
    components->port = gurl.port();

  if (gurl.has_query())
    components->query = gurl.query();

  if (gurl.has_ref())
    components->fragment = gurl.ref();

  return true;
}

uint32_t AdsServiceImpl::SetTimer(const uint64_t& time_offset) {
  if (next_timer_id_ == std::numeric_limits<uint32_t>::max())
    next_timer_id_ = 1;
  else
    ++next_timer_id_;

  timers_[next_timer_id_] = std::make_unique<base::OneShotTimer>();
  timers_[next_timer_id_]->Start(FROM_HERE,
      base::TimeDelta::FromSeconds(time_offset),
      base::BindOnce(
          &AdsServiceImpl::OnTimer, AsWeakPtr(), next_timer_id_));

  return next_timer_id_;
}

void AdsServiceImpl::KillTimer(uint32_t timer_id) {
  if (timers_.find(timer_id) == timers_.end())
    return;

  timers_[timer_id]->Stop();
  timers_.erase(timer_id);
}

void AdsServiceImpl::OnTimer(uint32_t timer_id) {
  if (!ads_)
    return;

  timers_.erase(timer_id);
  ads_->OnTimer(timer_id);
}

std::ostream& AdsServiceImpl::Log(const char* file,
                                  int line,
                                  const ads::LogLevel log_level) const {
  switch(log_level) {
    case ads::LogLevel::INFO:
      return logging::LogMessage(file, line, logging::LOG_INFO).stream();
      break;
    case ads::LogLevel::WARNING:
      return logging::LogMessage(file, line, logging::LOG_WARNING).stream();
      break;
    default:
      return logging::LogMessage(file, line, logging::LOG_ERROR).stream();
  }
}

}  // namespace brave_ads