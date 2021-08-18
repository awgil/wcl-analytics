module;

#pragma warning(push)
#pragma warning(disable : 4715)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <iostream> // debug TODO remove

// this module contains WCL schema and utilities to fetch data according to this schema
export module wcl.fetcher;
export import wcl.connection;

export {
	using Talents = std::array<int, 3>; // num points per spec, unfortunately no details available :(

	struct ReportActor
	{
		int gameID;
		int petOwnerID = -1; // -1 if not a pet
		std::string name;
		std::string type;
		std::string subtype;
	};

	struct ReportAbility
	{
		int gameID;
		std::string name;
		std::string type;
	};

	struct ReportFightNPC
	{
		int id; // in report's actor list
		int instanceCount;
		int groupCount;
	};

	struct ReportFight
	{
		int id;
		int encounterID;
		int startTime;
		int endTime;
		std::string name;
		std::vector<ReportFightNPC> enemyNPCs;
	};

	struct Report
	{
		std::string code;
		std::string title;
		std::vector<ReportActor> actors;
		std::vector<ReportAbility> abilities;
		std::vector<ReportFight> fights;
	};

	struct EventActor
	{
		int actorID;
		int actorInstance = 0;

		friend auto operator<=>(const EventActor&, const EventActor&) = default;
	};

	struct Event
	{
		struct CombatantInfo
		{
			int actorID;
			bool valid;
			Talents talents;
		};

		struct AuraChange
		{
			bool isDebuff;
			bool isApply; // true for 'apply', false for 'remove' & 'refresh' events
			bool isRemove; // true for 'remove', false for 'apply' & 'refresh' events
			int abilityGameID;
			EventActor source;
			EventActor target;
			int stacks = 0; // > 0 for '*stack' events
		};

		enum HitType { Miss, Hit, Crit, FullAbsorb, Block, CritBlock, Glancing, Dodge, Parry, Unk9, Immune, Unk11, Unk12, Evade, SpellMiss, Crush, Tick, SpellCrit };

		struct Damage
		{
			int abilityGameID;
			EventActor source;
			EventActor target;
			int amountUnmit;
			int amountFinal;
			int amountResisted; // ??? often looks like 25/50/75% of unmit??
			int amountBlocked; // ???
			int amountAbsorbed; // ??? hypothesis: unmit -> post-mit - absorb = final (fullabsorb has absorb>unmit and final=0)
			HitType hitType;
		};

		int timestamp;
		std::variant<CombatantInfo, AuraChange, Damage> data;
	};

	using CombatantInfo = std::unordered_map<int, Talents>; // key = actor ID

	class WCLFetcher : public WCLConnection
	{
	public:
		auto fetchReportsForZone(int zone)
		{
			// note: uncacheable, since new reports can get uploaded...
			std::vector<std::string> codes;
			int page = 1;
			while (true)
			{
				auto q = query("{reportData{reports(zoneID:" + std::to_string(zone) + " page:" + std::to_string(page) + "){data{code masterData{gameVersion logVersion}} has_more_pages}}}", true);
				auto& reports = q["reportData"]["reports"];

				for (auto& report : reports["data"])
				{
					if (report["masterData"]["gameVersion"].get<int>() == 1)
						codes.push_back(report["code"].get<std::string>());
					else
						__debugbreak();
				}

				if (reports["has_more_pages"].get<bool>())
					++page;
				else
					break;
			}
			return codes;
		}

		auto queryReport(const std::string& code, const std::string& fields)
		{
			auto q = query("{reportData{report(code:\"" + code + "\"){" + fields + "}}}");
			return std::move(q["reportData"]["report"]);
		}

		auto fetchReport(const std::string& code)
		{
			auto report = queryReport(code, "title fights{encounterID id name startTime endTime enemyNPCs{id instanceCount groupCount}} masterData{abilities{gameID name type} actors{id gameID petOwner name type subType}}");
			auto& masterData = report["masterData"];

			auto res = Report{ code, report["title"].get<std::string>() };
			for (auto& actor : masterData["actors"])
			{
				assert(actor["id"].get<int>() == (res.actors.empty() ? -1 : static_cast<int>(res.actors.size())));
				auto& petOwner = actor["petOwner"];
				res.actors.push_back({ actor["gameID"].get<int>(), petOwner.is_null() ? -1 : petOwner.get<int>(), actor["name"].get<std::string>(), actor["type"].get<std::string>(), actor["subType"].get<std::string>() });
			}
			for (auto& ability : masterData["abilities"])
			{
				res.abilities.push_back({ ability["gameID"].get<int>(), ability["name"].get<std::string>(), ability["type"].get<std::string>() });
			}
			for (auto& fight : report["fights"])
			{
				std::vector<ReportFightNPC> enemyNPCs;
				for (auto& npc : fight["enemyNPCs"])
					enemyNPCs.push_back({ npc["id"].get<int>(), npc["instanceCount"].get<int>(), npc["groupCount"].get<int>() });
				res.fights.push_back({ fight["id"].get<int>(), fight["encounterID"].get<int>(), fight["startTime"].get<int>(), fight["endTime"].get<int>(), fight["name"].get<std::string>(), std::move(enemyNPCs) });
			}
			return res;
		}

		auto fetchEvents(const std::string& code, int startTime, int endTime, const std::string& filters = {})
		{
			static auto handlers = []() {
				std::unordered_map<std::string, void(*)(std::vector<Event>&, int, nlohmann::json&)> res;
				res["encounterstart"] = ignoreEvent;
				res["encounterend"] = ignoreEvent;
				res["combatantinfo"] = combatantInfoEvent;
				res["applybuff"] = auraEvent<false, true, false, false>;
				res["applydebuff"] = auraEvent<true, true, false, false>;
				res["applybuffstack"] = auraEvent<false, true, false, true>;
				res["applydebuffstack"] = auraEvent<true, true, false, true>;
				res["removebuff"] = auraEvent<false, false, true, false>;
				res["removedebuff"] = auraEvent<true, false, true, false>;
				res["removebuffstack"] = auraEvent<false, false, true, true>;
				res["removedebuffstack"] = auraEvent<true, false, true, true>;
				res["refreshbuff"] = auraEvent<false, false, false, false>;
				res["refreshdebuff"] = auraEvent<true, false, false, false>;
				res["aurabroken"] = ignoreEvent;
				res["damage"] = damageEvent;
				res["extraattacks"] = ignoreEvent;
				res["heal"] = ignoreEvent;
				res["absorbed"] = ignoreEvent;
				res["energize"] = ignoreEvent;
				res["begincast"] = ignoreEvent;
				res["cast"] = ignoreEvent;
				res["summon"] = ignoreEvent;
				res["create"] = ignoreEvent;
				res["death"] = ignoreEvent;
				res["destroy"] = ignoreEvent;
				return res;
			}();

			std::vector<Event> res;
			while (true)
			{
				auto report = queryReport(code, "events(startTime:" + std::to_string(startTime) + " endTime:" + std::to_string(endTime) + " " + filters + "){data nextPageTimestamp}");
				auto& events = report["events"];
				for (auto& event : events["data"])
				{
					auto type = event["type"].get<std::string>();
					auto& h = handlers[type];
					if (!h)
					{
						std::cout << "Error: unhandled event type: " << type << std::endl;
						h = ignoreEvent;
					}
					h(res, event["timestamp"].get<int>(), event);
				}

				if (auto& next = events["nextPageTimestamp"]; !next.is_null())
					startTime = next.get<int>();
				else
					break;
			}
			return res;
		}

		// TODO: this is a hack, i've seen logs marked as TBCC which are actually from shadowlands :(
		std::optional<CombatantInfo> fetchCombatantInfo(const std::string& code, const ReportFight& fight)
		{
			auto combatants = fetchEvents(code, fight.startTime, fight.endTime, "dataType:CombatantInfo");
			CombatantInfo res;
			for (auto& event : combatants)
			{
				auto& info = std::get<Event::CombatantInfo>(event.data);
				if (!info.valid)
					return {};
				res[info.actorID] = info.talents;
			}
			return res;
		}

	private:
		static EventActor parseEventSource(nlohmann::json& j)
		{
			auto& inst = j["sourceInstance"];
			return EventActor{ j["sourceID"].get<int>(), inst.is_null() ? 0 : inst.get<int>() };
		}

		static EventActor parseEventTarget(nlohmann::json& j)
		{
			auto& inst = j["targetInstance"];
			return EventActor{ j["targetID"].get<int>(), inst.is_null() ? 0 : inst.get<int>() };
		}

		static void ignoreEvent(std::vector<Event>&, int, nlohmann::json&) {}

		static void combatantInfoEvent(std::vector<Event>& events, int ts, nlohmann::json& j)
		{
			Event::CombatantInfo res;
			res.actorID = j["sourceID"].get<int>();
			res.valid = j["expansion"].get<std::string>() == "tbc";
			if (res.valid)
			{
				int i = 0;
				for (auto& t : j["talents"])
					res.talents[i++] = t["id"].get<int>();
				assert(i == 3);
			}
			events.push_back({ ts, res });
		}

		static void damageEvent(std::vector<Event>& events, int ts, nlohmann::json& j)
		{
			static const auto getOptionalInt = [](nlohmann::json& j, const char* field) {
				auto& f = j[field];
				return f.is_null() ? 0 : f.get<int>();
			};

			Event::Damage res;
			res.abilityGameID = j["abilityGameID"].get<int>();
			res.source = parseEventSource(j);
			res.target = parseEventTarget(j);
			res.amountUnmit = getOptionalInt(j, "unmitigatedAmount");
			res.amountFinal = j["amount"].get<int>();
			res.amountResisted = getOptionalInt(j, "resisted");
			res.amountBlocked = getOptionalInt(j, "blocked");
			res.amountAbsorbed = getOptionalInt(j, "absorbed");
			res.hitType = static_cast<Event::HitType>(j["hitType"].get<int>());
			events.push_back({ ts, res });

			// debug
			//if (res.amountUnmit - getOptionalInt(j, "mitigated") != res.amountFinal)
			//{
			//	std::cout << "mit+unmit mismatch: " << j.dump(1) << std::endl;
			//}
			if (res.hitType != Event::Miss && res.hitType != Event::Hit && res.hitType != Event::Crit && res.hitType != Event::Block && res.hitType != Event::Glancing && res.hitType != Event::Crush && res.hitType != Event::Dodge && res.hitType != Event::Parry
				&& res.hitType != Event::Immune && res.hitType != Event::SpellMiss && res.hitType != Event::Tick && res.hitType != Event::SpellCrit && res.hitType != Event::FullAbsorb && res.hitType != Event::Evade && res.hitType != Event::CritBlock)
			{
				std::cout << "investigate hittype: " << j.dump(1) << std::endl;
			}
			if (res.hitType == Event::SpellCrit && res.amountResisted == 0)
			{
				std::cout << "unresisted spell crit: " << j.dump(1) << std::endl;
			}
			if (res.hitType == Event::FullAbsorb && (res.amountAbsorbed == 0 || res.amountFinal != 0))
			{
				std::cout << "strange absorb: " << j.dump(1) << std::endl;
			}
			if (res.hitType == Event::CritBlock && res.amountBlocked == 0)
			{
				std::cout << "strange critblock: " << j.dump(1) << std::endl;
			}
			//j["tick"]; // bool
			//j["sourceMarker"]; j["targetMarker"];
			//j["overkill"];
			//j["mitigated"];
			//if (j.size() != 18)
			//{
			//	std::cout << "extra fields: " << j.dump(1) << std::endl;
			//}
		}

		template<bool IsDebuff, bool IsApply, bool IsRemove, bool IsStack>
		static void auraEvent(std::vector<Event>& events, int ts, nlohmann::json& j)
		{
			Event::AuraChange res{ IsDebuff, IsApply, IsRemove };
			res.abilityGameID = j["abilityGameID"].get<int>();
			res.source = parseEventSource(j);
			res.target = parseEventTarget(j);
			if constexpr (IsStack)
			{
				res.stacks = j["stack"].get<int>();
				assert(res.stacks > 0);
			}
			events.push_back({ ts, res });
		}
	};
}
