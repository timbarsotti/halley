#include "xbl_manager.h"
#include "halley/support/logger.h"
#include "halley/text/halleystring.h"
#include "halley/concurrency/concurrent.h"

#include <map>

#include <vccorlib.h>
#include <winrt/base.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Gaming.XboxLive.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include "xsapi/services.h"
#include <ppltasks.h>

using namespace Halley;

template <typename T>
T from_cx(Platform::Object^ from)
{
    T to{ nullptr };

    winrt::check_hresult(reinterpret_cast<::IUnknown*>(from)
        ->QueryInterface(winrt::guid_of<T>(),
            reinterpret_cast<void**>(winrt::put_abi(to))));

    return to;
}

template <typename T>
T^ to_cx(winrt::Windows::Foundation::IUnknown const& from)
{
    return safe_cast<T^>(reinterpret_cast<Platform::Object^>(winrt::get_abi(from)));
}

XBLManager::XBLManager()
{
	
}

XBLManager::~XBLManager()
{
	deInit();
}

void XBLManager::init()
{
	signIn();
}

void XBLManager::deInit()
{
	achievementsStatus = XBLAchievementsStatus::Uninitialized;
	achievementStatus.clear();
}

std::shared_ptr<ISaveData> XBLManager::getSaveContainer(const String& name)
{
	auto iter = saveStorage.find(name);
	if (iter == saveStorage.end()) {
		auto save = std::make_shared<XBLSaveData>(*this, name);
		saveStorage[name] = save;
		return save;
	} else {
		return iter->second;
	}
}

void XBLManager::recreateCloudSaveContainer()
{
	if (status == XBLStatus::Connected)
	{
		Concurrent::execute([=]() -> void
		{
			gameSaveProvider.reset();
			status = XBLStatus::Disconnected;
			getConnectedStorage().get();

			std::map<String, std::shared_ptr<ISaveData>>::iterator iter;
			for (iter = saveStorage.begin(); iter != saveStorage.end(); ++iter)
			{
				std::dynamic_pointer_cast<XBLSaveData>(iter->second)->recreate();
			}
		}).get();
	}
}

Maybe<winrt::Windows::Gaming::XboxLive::Storage::GameSaveProvider> XBLManager::getProvider() const
{
	return gameSaveProvider;
}

XBLStatus XBLManager::getStatus() const
{
	return status;
}

class XboxLiveAuthorisationToken : public AuthorisationToken {
public:
	XboxLiveAuthorisationToken(String gamertag, String userId, String token)
	{
		data["gamertag"] = std::move(gamertag);
		data["userId"] = std::move(userId);
		data["token"] = std::move(token);
	}

	String getType() const override
	{
		return "xboxlive";
	}

	bool isSingleUse() const override
	{
		return false;
	}

	bool isCancellable() const override
	{
		return false;
	}

	void cancel() override
	{	
	}

	std::map<String, String> getMapData() const override
	{
		return data;
	}

private:
	std::map<String, String> data;
	bool playOnline = false;
	bool shareUGC = false;
};

Future<AuthTokenResult> XBLManager::getAuthToken(const AuthTokenParameters& parameters)
{
	Promise<AuthTokenResult> promise;
	if (status == XBLStatus::Connected) {
		auto future = promise.getFuture();

  		xboxLiveContext->user()->get_token_and_signature(parameters.method.getUTF16().c_str(), parameters.url.getUTF16().c_str(), parameters.headers.getUTF16().c_str())
			.then([=, promise = std::move(promise)](xbox::services::xbox_live_result<xbox::services::system::token_and_signature_result> result) mutable
 		{
			if (result.err()) {
				Logger::logError(result.err_message());
				promise.setValue(AuthTokenRetrievalResult::Error);
			} else {
				auto payload = result.payload();
				auto privileges = String(payload.privileges().c_str());
				auto gamerTag = String(payload.gamertag().c_str());
				auto userId = String(payload.xbox_user_id().c_str());
				auto token = String(payload.token().c_str());

				OnlineCapabilities capabilities;
				for (const auto& priv: privileges.split(' ')) {
					const int privNumber = priv.toInteger();
					if (privNumber == 254) { // XPRIVILEGE_MULTIPLAYER_SESSIONS
						capabilities.onlinePlay = true;
					} else if (privNumber == 247) { // XPRIVILEGE_USER_CREATED_CONTENT
						capabilities.ugc = true;
					}
				}

				promise.setValue(AuthTokenResult(std::make_unique<XboxLiveAuthorisationToken>(gamerTag, userId, token), capabilities));
			}
		});

		return future;
	} else {
		promise.setValue(AuthTokenRetrievalResult::Error);
		return promise.getFuture();
	}
}

void XBLManager::setAchievementProgress(const String& achievementId, int currentProgress, int maximumValue)
{
	if (xboxUser != nullptr && xboxLiveContext != nullptr)
	{
		string_t id (achievementId.cppStr().begin(), achievementId.cppStr().end());
		int progress = (int)floor(((float)currentProgress / (float)maximumValue) * 100.f);
		xboxLiveContext->achievement_service().update_achievement(xboxUser->xbox_user_id(), id, progress).then([=] (xbox::services::xbox_live_result<void> result)
		{ 
			if (result.err())
			{
				Logger::logError(String("Error unlocking achievement '") + achievementId + String("': ") + result.err().value() + " "  + result.err_message());
			}
			else if (progress == 100)
			{
				achievementStatus[id] = true;
			}
		});
	}
}

bool XBLManager::isAchievementUnlocked(const String& achievementId, bool defaultValue)
{
	if (achievementsStatus == XBLAchievementsStatus::Uninitialized)
	{
		Logger::logWarning(String("Trying to get the achievement status before starting the retrieve task!"));
		return false;
	}
	else if (achievementsStatus == XBLAchievementsStatus::Retrieving)
	{
		unsigned long long timeout = GetTickCount64() + 5000;
		while (achievementsStatus == XBLAchievementsStatus::Retrieving && GetTickCount64() < timeout) {}

		if (achievementsStatus == XBLAchievementsStatus::Retrieving)
		{
			Logger::logWarning(String("Achievements are taking too long to load!"));
			return false;
		}
	}

	string_t id(achievementId.cppStr().begin(), achievementId.cppStr().end());
	auto iterator = achievementStatus.find(id);
	if (iterator != achievementStatus.end())
	{
		return iterator->second;
	}
	return defaultValue;
}

String XBLManager::getPlayerName()
{
	if (xboxUser)
	{
		return String(xboxUser->gamertag().c_str());
	}
	return "";
}

void XBLManager::signIn()
{
	xbox::services::system::xbox_live_services_settings::get_singleton_instance()->set_diagnostics_trace_level(xbox::services::xbox_services_diagnostics_trace_level::verbose);
	status = XBLStatus::Connecting;

	using namespace xbox::services::system;
	xboxUser = std::make_shared<xbox_live_user>(nullptr);
	auto dispatcher = to_cx<Platform::Object>(winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher());

	auto onLoggedIn = [=] () -> winrt::Windows::Foundation::IAsyncAction
	{
		xboxLiveContext = std::make_shared<xbox::services::xbox_live_context>(xboxUser);

		xbox_live_user::add_sign_out_completed_handler([this](const sign_out_completed_event_args&)
		{
			xboxUser.reset();
			xboxLiveContext.reset();
			gameSaveProvider.reset();
			status = XBLStatus::Disconnected;
			achievementsStatus = XBLAchievementsStatus::Uninitialized;
			achievementStatus.clear();
		});

		retrieveUserAchievementsState();

		co_await getConnectedStorage();
	};

	xboxUser->signin_silently(dispatcher).then([=] (xbox::services::xbox_live_result<sign_in_result> result) -> winrt::Windows::Foundation::IAsyncAction
	{
		if (result.err()) {
			Logger::logError(String("Error signing in to Xbox Live: ") + result.err_message());
			status = XBLStatus::Disconnected;
		} else {
			auto resultStatus = result.payload().status();
			switch (resultStatus) {
			case success:
				co_await onLoggedIn();
				break;

			case user_interaction_required:
				xboxUser->signin(dispatcher).then([&](xbox::services::xbox_live_result<sign_in_result> loudResult) -> winrt::Windows::Foundation::IAsyncAction
                {
                    if (loudResult.err()) {
						Logger::logError("Error signing in to Xbox live: " + String(loudResult.err_message().c_str()));
					} else {
                        auto resPayload = loudResult.payload();
                        switch (resPayload.status()) {
                        case success:
							co_await onLoggedIn();
                            break;
						default:
							status = XBLStatus::Disconnected;
                            break;
                        }
                    }
                }, concurrency::task_continuation_context::use_current());
				break;

			default:
				status = XBLStatus::Disconnected;
			}
		}
	});
}

winrt::Windows::Foundation::IAsyncAction XBLManager::getConnectedStorage()
{
	using namespace winrt::Windows::Gaming::XboxLive::Storage;
	
	auto windowsUser = co_await winrt::Windows::System::User::FindAllAsync();

	GameSaveProviderGetResult result = co_await GameSaveProvider::GetForUserAsync(*windowsUser.First(), xboxLiveContext->application_config()->scid());

	if (result.Status() == GameSaveErrorStatus::Ok) {
		gameSaveProvider = result.Value();
		status = XBLStatus::Connected;
	} else {
		status = XBLStatus::Disconnected;
	}
}

void XBLManager::retrieveUserAchievementsState()
{
	achievementsStatus = XBLAchievementsStatus::Retrieving;
	achievementStatus.clear();

	xboxLiveContext->achievement_service().get_achievements_for_title_id(
		xboxUser->xbox_user_id(),
		xboxLiveContext->application_config()->title_id(),
		xbox::services::achievements::achievement_type::all,
		false,
		xbox::services::achievements::achievement_order_by::title_id,
		0,
		0)
		.then([=](xbox::services::xbox_live_result<xbox::services::achievements::achievements_result> result)
	{
		try
		{
			bool receivedMoreAchievements;
			do
			{
				receivedMoreAchievements = false;
				if (result.err())
				{
					Logger::logError(String("Error retrieving achievements for user '") + xboxUser->gamertag().c_str() + String("': ") + result.err().value() + " " + result.err_message());
					achievementsStatus = XBLAchievementsStatus::Uninitialized;
				}
				else
				{
					std::vector<xbox::services::achievements::achievement> achievements = result.payload().items();
					for (unsigned int i = 0; i < achievements.size(); ++i)
					{
						xbox::services::achievements::achievement achievement = achievements[i];
						bool isAchieved = (achievement.progress_state() == xbox::services::achievements::achievement_progress_state::achieved);
						Logger::logInfo(String("Achievement '") + achievement.name().c_str() + String("' (ID '") + achievement.id().c_str() + String("'): ") + (isAchieved ? String("Achieved") : String("Locked")));
						achievementStatus[achievement.id()] = isAchieved;
					}

					if (result.payload().has_next())
					{
						result = result.payload().get_next(32).get();
						receivedMoreAchievements = true;
					}
					else
					{
						achievementsStatus = XBLAchievementsStatus::Ready;
					}
				}
			} while (receivedMoreAchievements);
		}
		catch (...)
		{
			achievementsStatus = XBLAchievementsStatus::Uninitialized;
			Logger::logError(String("Error retrieving achievements for user '") + xboxUser->gamertag().c_str() + String("'"));
		}
	});
}

void XBLManager::showPlayerInfo(String playerId)
{
	xbox::services::system::title_callable_ui::show_profile_card_ui(playerId.getUTF16());
}

XBLSaveData::XBLSaveData(XBLManager& manager, String containerName)
	: manager(manager)
	, containerName(containerName.isEmpty() ? "save" : containerName)
	, isSaving(false)
{
	updateContainer();
}

bool XBLSaveData::isReady() const
{
	updateContainer();
	return gameSaveContainer.is_initialized();
}

Bytes XBLSaveData::getData(const String& path)
{
	if (!isReady()) {
		throw Exception("Container is not ready yet!", HalleyExceptions::PlatformPlugin);
	}

	return Concurrent::execute([&] () -> Bytes
	{
		if (isSaving)
		{
			unsigned long long timeout = GetTickCount64() + 3000;
			while (isSaving && GetTickCount64() < timeout) {}

			if (isSaving)
			{
				Logger::logWarning(String("Saving data to connected storage is taking too long!"));
			}
		}

		auto key = winrt::hstring(path.getUTF16());
		std::vector<winrt::hstring> updates;
		updates.push_back(key);
		auto view = winrt::single_threaded_vector(std::move(updates)).GetView();

		auto gameBlob = gameSaveContainer->GetAsync(view).get();

		if (gameBlob.Status() == winrt::Windows::Gaming::XboxLive::Storage::GameSaveErrorStatus::Ok) {
			if (gameBlob.Value().HasKey(key)) {
				auto buffer = gameBlob.Value().Lookup(key);

				auto size = buffer.Length();
				Bytes result(size);
				auto dataReader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
				dataReader.ReadBytes(winrt::array_view<uint8_t>(result));

				return result;
			}
		}
		else
		{
			Logger::logError(String("Error getting Blob '") + path + String("': ") + (int)gameBlob.Status());
		}

		return {};
	}).get();
}

std::vector<String> XBLSaveData::enumerate(const String& root)
{
	if (!isReady()) {
		throw Exception("Container is not ready yet!", HalleyExceptions::PlatformPlugin);
	}

	return Concurrent::execute([&] () -> std::vector<String>
	{
		std::vector<String> results;

		auto query = gameSaveContainer->CreateBlobInfoQuery(root.getUTF16().c_str());
		auto info = query.GetBlobInfoAsync().get();
		if (info.Status() == winrt::Windows::Gaming::XboxLive::Storage::GameSaveErrorStatus::Ok) {
			auto& entries = info.Value();
			for (uint32_t i = 0; i < entries.Size(); ++i) {
				results.push_back(String(entries.GetAt(i).Name().c_str()));
			}
		}

		return results;
	}).get();
}

void XBLSaveData::setData(const String& path, const Bytes& data, bool commit)
{
	if (!isReady()) {
		throw Exception("Container is not ready yet!", HalleyExceptions::PlatformPlugin);
	}

	isSaving = true;
	Concurrent::execute([=]() -> void
	{
		auto dataWriter = winrt::Windows::Storage::Streams::DataWriter();
		dataWriter.WriteBytes(winrt::array_view<const uint8_t>(data));

		std::map<winrt::hstring, winrt::Windows::Storage::Streams::IBuffer> updates;
		updates[winrt::hstring(path.getUTF16())] = dataWriter.DetachBuffer();
		auto view = winrt::single_threaded_map(std::move(updates)).GetView();

		auto result = gameSaveContainer->SubmitUpdatesAsync(view, {}, L"").get();
		if (result.Status() != winrt::Windows::Gaming::XboxLive::Storage::GameSaveErrorStatus::Ok)
		{
			Logger::logError(String("Error saving Blob '") + path + String("': ") + (int)result.Status());
		}

		isSaving = false;
	});
}

void XBLSaveData::removeData(const String& path)
{
	if (!isReady()) {
		throw Exception("Container is not ready yet!", HalleyExceptions::PlatformPlugin);
	}

	Concurrent::execute([=]() -> void
	{
		auto key = winrt::hstring(path.getUTF16());
		std::vector<winrt::hstring> updates;
		updates.push_back(key);
		auto view = winrt::single_threaded_vector(std::move(updates)).GetView();

		auto result = gameSaveContainer->SubmitUpdatesAsync({}, view, L"").get();
		if (result.Status() != winrt::Windows::Gaming::XboxLive::Storage::GameSaveErrorStatus::Ok)
		{
			Logger::logError(String("Error deleting Blob '") + path + String("': ") + (int)result.Status());
		}
	}).get();
}

void XBLSaveData::commit()
{
	
}

void XBLSaveData::recreate()
{
	gameSaveContainer.reset();
	gameSaveContainer = manager.getProvider()->CreateContainer(containerName.getUTF16().c_str());
}

void XBLSaveData::updateContainer() const
{
	if (manager.getStatus() == XBLStatus::Connected) {
		if (!gameSaveContainer) {
			gameSaveContainer = manager.getProvider()->CreateContainer(containerName.getUTF16().c_str());
		}
	} else {
		gameSaveContainer.reset();
	}
}
