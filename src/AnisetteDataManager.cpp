#include "AnisetteDataManager.h"
#include <sstream>
#include <filesystem>
#include "Error.hpp"
#include "ServerError.hpp"

#include <set>
#include <ctime>
#include <cstdlib>

#include "AnisetteData.h"
#include "AltServerApp.h"

AnisetteDataManager* AnisetteDataManager::_instance = nullptr;

AnisetteDataManager* AnisetteDataManager::instance()
{
	if (_instance == 0)
	{
		_instance = new AnisetteDataManager();
	}

	return _instance;
}

AnisetteDataManager::AnisetteDataManager() : loadedDependencies(false)
{
}

AnisetteDataManager::~AnisetteDataManager()
{
}

bool AnisetteDataManager::LoadiCloudDependencies()
{
	return true;
}

bool AnisetteDataManager::LoadDependencies()
{
	return true;
}

#include <cpprest/json.h>

using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features

std::shared_ptr<AnisetteData> AnisetteDataManager::FetchAnisetteData()
{
	// The anisette server is overridable via ALTSERVER_ANISETTE_SERVER so a
	// self-hosted anisette-v3-server can replace the historical public
	// armconverter.com default — a flaky third party that returns 502 when its
	// origin is down. A v3 server returns the same header JSON from its flat
	// "GET /" endpoint, so only the base URL and the request path differ.
	const char* anisetteEnv = std::getenv("ALTSERVER_ANISETTE_SERVER");
	bool useConfiguredAnisette = (anisetteEnv != nullptr && anisetteEnv[0] != '\0');
	std::string anisetteHost = useConfiguredAnisette ? std::string(anisetteEnv) : std::string("https://armconverter.com");
	std::string wideURI = useConfiguredAnisette ? std::string("/") : std::string("/anisette/irGb3Quww8zrhgqnzmrx");
	
	auto encodedURI = web::uri::encode_uri(wideURI);
	uri_builder builder(encodedURI);

	http_request request(methods::GET);
	request.set_request_uri(builder.to_string());
	
	std::map<utility::string_t, utility::string_t> headers = {
		{"User-Agent", "Xcode"},
	};

	for (auto& pair : headers)
	{
		if (request.headers().has(pair.first))
		{
			request.headers().remove(pair.first);
		}

		request.headers().add(pair.first, pair.second);
	}

	std::shared_ptr<AnisetteData> anisetteData = NULL;

	auto client = web::http::client::http_client(anisetteHost);
	auto task = client.request(request)
		.then([=](http_response response)
			{
				return response.content_ready();
			})
		.then([=](http_response response)
			{
				odslog("Received response status code: " << response.status_code());
				return response.extract_json();
			})
		.then([&anisetteData](pplx::task<json::value> previousTask)
			{
				odslog("parse anisette data ret");
				json::value jsonVal = previousTask.get();
				odslog("Got anisetteData json: " << jsonVal);
				std::vector<std::string> keys = {
					"X-Apple-I-MD-M",
					"X-Apple-I-MD",
					"X-Apple-I-MD-LU",
					"X-Apple-I-MD-RINFO",
					"X-Mme-Device-Id",
					"X-Apple-I-SRL-NO",
					"X-MMe-Client-Info",
					"X-Apple-I-Client-Time",
					"X-Apple-Locale",
					"X-Apple-I-TimeZone"
				};
				for (auto &key : keys) {
					odslog(key << ": " << jsonVal.at(key).as_string().c_str());
				}

				struct tm tm = { 0 };
				strptime(jsonVal.at("X-Apple-I-Client-Time").as_string().c_str(), "%FT%T%z", &tm);
				time_t ts = mktime(&tm);
				struct timeval tv = {0};
				tv.tv_sec = ts;

				odslog("Building anisetteData obj...");
				anisetteData = std::make_shared<AnisetteData>(
					jsonVal.at("X-Apple-I-MD-M").as_string(),
					jsonVal.at("X-Apple-I-MD").as_string(),
					jsonVal.at("X-Apple-I-MD-LU").as_string(),
					std::atoi(jsonVal.at("X-Apple-I-MD-RINFO").as_string().c_str()),
					jsonVal.at("X-Mme-Device-Id").as_string(),
					jsonVal.at("X-Apple-I-SRL-NO").as_string(),
					jsonVal.at("X-MMe-Client-Info").as_string(),
					tv,
					jsonVal.at("X-Apple-Locale").as_string(),
					jsonVal.at("X-Apple-I-TimeZone").as_string());

				//IterateJSONValue();
			});
	
	task.wait();

	odslog(*anisetteData);

	return anisetteData;
}

bool AnisetteDataManager::ReprovisionDevice(std::function<void(void)> provisionCallback)
{
#if !SPOOF_MAC
	provisionCallback();
	return true;
#else
	std::string adiDirectoryPath = "C:\\ProgramData\\Apple Computer\\iTunes\\adi";

	/* Start Provisioning */

	// Move iCloud's ADI files (so we don't mess with them).
	for (const auto& entry : fs::directory_iterator(adiDirectoryPath))
	{
		if (entry.path().extension() == ".pb")
		{
			fs::path backupPath = entry.path();
			backupPath += ".icloud";

			fs::rename(entry.path(), backupPath);
		}
	}

	// Copy existing AltServer .pb files into original location to reuse the MID.
	for (const auto& entry : fs::directory_iterator(adiDirectoryPath))
	{
		if (entry.path().extension() == ".altserver")
		{
			fs::path path = entry.path();
			path.replace_extension();

			fs::rename(entry.path(), path);
		}
	}

	auto cleanUp = [adiDirectoryPath]() {
		/* Finish Provisioning */

		// Backup AltServer ADI files.
		for (const auto& entry : fs::directory_iterator(adiDirectoryPath))
		{
			// Backup AltStore file
			if (entry.path().extension() == ".pb")
			{
				fs::path backupPath = entry.path();
				backupPath += ".altserver";

				fs::rename(entry.path(), backupPath);
			}
		}

		// Copy iCloud ADI files back to original location.
		for (const auto& entry : fs::directory_iterator(adiDirectoryPath))
		{
			if (entry.path().extension() == ".icloud")
			{
				// Move backup file to original location
				fs::path path = entry.path();
				path.replace_extension();

				fs::rename(entry.path(), path);

				odslog("Copying iCloud file from: " << entry.path().string() << " to: " << path.string());
			}
		}
	};

	// Calling CopyAnisetteData implicitly generates new anisette data,
	// using the new client info string we injected.
	ObjcObject* error = NULL;
	ObjcObject* anisetteDictionary = (ObjcObject*)CopyAnisetteData(NULL, 0x1, &error);

	try
	{
		if (anisetteDictionary == NULL)
		{
			odslog("Reprovision Error:" << ((ObjcObject*)error)->description());

			ObjcObject* localizedDescription = (ObjcObject*)((id(*)(id, SEL))objc_msgSend)(error, sel_registerName("localizedDescription"));
			if (localizedDescription)
			{
				int errorCode = ((int(*)(id, SEL))objc_msgSend)(error, sel_registerName("code"));
				throw LocalizedError(errorCode, localizedDescription->description());
			}
			else
			{
				throw ServerError(ServerErrorCode::InvalidAnisetteData);
			}
		}

		odslog("Reprovisioned Anisette:" << anisetteDictionary->description());

		AltServerApp::instance()->setReprovisionedDevice(true);

		// Call callback while machine is provisioned for AltServer.
		provisionCallback();
	}
	catch (std::exception &exception)
	{
		cleanUp();

		throw;
	}

	cleanUp();

	return true;
#endif
}

bool AnisetteDataManager::ResetProvisioning()
{
	std::string adiDirectoryPath = "C:\\ProgramData\\Apple Computer\\iTunes\\adi";

	// Remove existing AltServer .pb files so we can create new ones next time we provision this device.
	for (const auto& entry : fs::directory_iterator(adiDirectoryPath))
	{
		if (entry.path().extension() == ".altserver")
		{
			fs::remove(entry.path());
		}
	}

	return true;
}