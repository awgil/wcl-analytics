module;

#pragma warning(push)
#pragma warning(disable : 4458)
#include <cpr/cpr.h>
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 4715)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <filesystem>
#include <format>
#include <iostream>
#include <ranges>
#include <string>

// this module contains low-level utilities for sending queries to WCL and caching results
export module wcl.connection;

namespace
{
	const std::string kClientID = "9426ddaf-327a-4b5e-82f5-f4c0b13f9993";
	const std::string kClientSecret = "O9cvnMmhIi9HtPXeEGtttCS3jVbUHXU57zOEL0iz";

	const std::string kURLAuth = "https://www.warcraftlogs.com/oauth/token";
	const std::string kURLPublic = "https://www.warcraftlogs.com/api/v2/client";

	const std::string kCachePrefix = ".cache/";
}

export class WCLConnection
{
public:
	auto query(const std::string& q, bool cache = true)
	{
		auto cacheFilename = std::format("{}{:016x}", kCachePrefix, std::hash<std::string>()(q));
		auto existingCache = nlohmann::json::array();
		if (cache)
		{
			std::filesystem::create_directory(kCachePrefix);
			std::ifstream reader(cacheFilename);
			if (reader.is_open())
			{
				existingCache = nlohmann::json::parse(reader);
				auto proj = [](nlohmann::json& j) -> auto& { return j["request"].get_ref<const std::string&>(); };
				auto i = std::ranges::find(existingCache, q, proj);
				if (i != existingCache.end())
					return std::move((*i)["response"]);
			}
		}

		ensureAuthorized();

		auto rq = cpr::Get(cpr::Url{ kURLPublic }, mAuthHeader, cpr::Parameters{ { "query", q } });
		auto jq = nlohmann::json::parse(rq.text);
		auto& jdata = jq["data"];
		assert(!jdata.is_null());

		if (cache)
		{
			nlohmann::json cacheEntry;
			cacheEntry["request"] = q;
			cacheEntry["response"] = jdata;
			existingCache.push_back(std::move(cacheEntry));
			std::ofstream writer(cacheFilename);
			writer << existingCache.dump(2);
		}

		return std::move(jdata);
	}

private:
	void ensureAuthorized()
	{
		if (!mAuthHeader.empty())
			return; // done already

		auto rAuth = cpr::Post(cpr::Url{ kURLAuth }, cpr::Authentication{ kClientID, kClientSecret }, cpr::Payload{ { "grant_type", "client_credentials" } });
		auto jAuth = nlohmann::json::parse(rAuth.text);
		mAuthHeader = cpr::Header{ { "Authorization", std::string("Bearer ") + jAuth["access_token"].get<std::string>() } };
	}

private:
	cpr::Header mAuthHeader;
};
