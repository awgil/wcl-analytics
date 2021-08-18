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
	anal.configNPC(21216, -16, 73, 600000); // hydross
	anal.configNPC(22035, -16, 71); // pure spawn of hydross
	anal.configNPC(22036, -8, 71); // tainted spawn of hydross

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

		if (!encounterCounts[623])
			continue;

		auto processHydrossFight = [&](const ReportFight& fight) {
			auto combatants = conn.fetchCombatantInfo(code, fight);
			if (!combatants)
			{
				std::cout << "-> fight " << fight.id << " has invalid combatants, skipping" << std::endl;
				return;
			}
			anal.analyzeFight(conn, report, fight, *combatants);
		};
		for (auto& fight : report.fights)
			if (fight.encounterID == 623)
				processHydrossFight(fight);
	}

	for (auto& [npc, data] : anal.data())
	{
		std::cout << std::format("{}: calculated range: [{}-{}] ({} crushes seen), based on {} events ({} outliers, {} crits, {} skipped due to suspect AP, {} skipped due to AP change in same tick)",
			npc, data.damageMin, data.damageMax, data.numSeenCrushes, data.numProcessedEvents, data.numOutliers, data.numIgnoredCrits, data.numSuspectAuraExpired, data.numSuspectAuraChange) << std::endl;
	}

	//auto rateLimit = conn.query("{rateLimitData{limitPerHour pointsSpentThisHour pointsResetIn}}", false);
	return 1;
}
