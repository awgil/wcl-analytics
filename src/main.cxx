import analysis.npc_swings;

#include <nlohmann/json.hpp>
#include <format>
#include <iostream>
#include <ranges>
#include <variant>

//auto xBuffs = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:Buffs");
//auto xDebuffs = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:Debuffs");
//auto xCasts = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:Casts");
//auto xDamageDone = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageDone");
//auto xDamageTaken = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageTaken");
//auto xDTF = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageTaken hostilityType:Friendlies");
//auto xDDE = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageDone hostilityType:Enemies");

int main()
{
	SwingAnalysis anal;
	//anal.configNPC(21216, -16, 73, 600000); // hydross
	//anal.configNPC(22035, -16, 71); // pure spawn of hydross
	//anal.configNPC(22036, -8, 71); // tainted spawn of hydross
	//anal.configNPC(21217, 1, 73); // lurker
	////anal.configNPC(21865, 1, 70); // coilfang ambusher (ranged)
	////anal.configNPC(21873, 1, 70); // colifang guardian (melee)
	anal.configNPC(21213, 1, 73); // morogrim
	//anal.configNPC(21920, 1, 71); // tidewalker lurker (murloc)
	anal.configNPC(21214, 1, 73); // fathom-lord karathress
	anal.configNPC(21964, 1, 71); // fathom-lord caribdis
	anal.configNPC(21965, 1, 71); // fathom-lord tidalvess
	anal.configNPC(21966, 1, 71); // fathom-lord sharkkis
	//anal.configNPC(21215, 1, 73); // leotheras
	//anal.configNPC(21806, 1, 71); // greyheart spellbinder (leotheras initial mob)
	//anal.configNPC(21212, 1, 73); // lady vashj
	//anal.configNPC(22055, 1, 70); // coilfang elite (naga)
	//anal.configNPC(22056, 1, 70); // coilfang strider
	//anal.configNPC(19514, 1, 73); // al'ar
	//anal.configNPC(19551, 1, 70); // ember of al'ar
	//anal.configNPC(19516, 1, 73); // void reaver
	//anal.configNPC(18805, 1, 73); // astromancer solarian
	// 18925 (agent): lvl 69-70
	//anal.configNPC(18806, 1, 72); // solarium priest
	// TODO: kael'thas

	WCLFetcher conn;
	auto sscTkReports = conn.fetchReportsForZone(1010);
	int processedReports = 0;
	for (auto& code : std::ranges::drop_view(sscTkReports, processedReports))
	{
		++processedReports;
		auto report = conn.fetchReport(code);

		std::unordered_map<int, int> encounterCounts;
		for (auto& fight : report.fights)
			if (fight.encounterID != 0)
				++encounterCounts[fight.encounterID];

		std::string msg;
		for (auto& [id, count] : encounterCounts)
		{
			if (!msg.empty())
				msg += ", ";
			msg += std::format("{} ({} fights)", id, count);
		}
		std::cout << "Processing report " << code << ": " << msg << std::endl;

		for (auto& fight : report.fights)
		{
			if (fight.encounterID == 0)
				continue; // skip trash fights...

			if (fight.encounterID == 733)
				continue; // skip kael'thas (TODO)

			if (fight.encounterID != 627)
				continue; // TODO: process other fights...

			auto combatants = conn.fetchCombatantInfo(code, fight);
			if (!combatants)
			{
				std::cout << "-> fight " << fight.id << " has invalid combatants, skipping" << std::endl;
				continue;
			}

			anal.analyzeFight(conn, report, fight, *combatants);
		}
	}

	for (auto& [npc, data] : anal.data())
	{
		std::cout << std::format("{}: calculated range: [{}-{}] ({} crushes seen), based on {} events ({} outliers, {} crits, {} skipped due to suspected expired AP auras, {} skipped due to AP change in same tick)",
			npc, data.damageMin, data.damageMax, data.numSeenCrushes, data.numProcessedEvents, data.numOutliers, data.numIgnoredCrits, data.numSuspectAuraExpired, data.numSuspectAuraChange) << std::endl;
	}

	//auto rateLimit = conn.query("{rateLimitData{limitPerHour pointsSpentThisHour pointsResetIn}}", false);
	return 1;
}
