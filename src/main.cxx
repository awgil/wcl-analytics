import analysis.npc_swings;

#include <nlohmann/json.hpp>
#include <format>
#include <iostream>
#include <variant>

// dmg formula: damage = multiplier * ( [5, 7.5] + AP / factor )
// AP = Level * 5.5 - 81.8 = 319.7 = 320
// factor = Level * 5.34 - 81.8 = 308.02
// damage = multiplier_max * (5 + APmin / factor) = multiplier_min * (7.5 + APmax / factor)
// => mul_min = damage / (7.5 + APmax / factor)

// alternative (x5): damage = basedmg * (1 + [0, 0.5] + AP / F), where F = factor*5 = 1540.1 @ L73
// alternative (x6.25): damage = basedmg * (1 + [-0.2, 0.2] + AP / F), where F = factor*6.25 == 1925.125 @ L73 (gives more outliers, even though it's more symmetrical, it's probably incorrect)

#if 0
auto detectMissingInitialAuras(const std::vector<Event>& events, int actorID)
{
	struct Aura { int source; int ability; bool active; };
	std::vector<Aura> auras;
	auto findAura = [&](const Event::AuraChange& aura) {
		auto proj = [](const Aura& a) { return std::make_pair(a.source, a.ability); };
		auto val = std::make_pair(aura.source.actorID, aura.abilityGameID);
		auto it = std::ranges::find(auras, val, proj);
		assert(it == auras.end() || std::ranges::find(it + 1, auras.end(), val, proj) == auras.end());
		return it;
	};

	std::vector<Event::AuraChange> res;
	for (auto& event : events)
	{
		if (!std::holds_alternative<Event::AuraChange>(event.data))
			continue;

		auto& aura = std::get<Event::AuraChange>(event.data);
		if (aura.target.actorID != actorID)
			continue; // not revelant

		if (aura.stacks != 0)
			continue; // ignore stacks?..

		auto it = findAura(aura);
		if (it == auras.end())
		{
			auras.push_back({ aura.source.actorID, aura.abilityGameID, !aura.isRemove });
			if (!aura.isApply)
			{
				// missing apply at the start of the fight - first event is remove or refresh
				res.push_back(aura);
				res.back().isApply = true;
				res.back().isRemove = false;
			}
		}
		else if (aura.isApply)
		{
			it->active = true; // note: it could already be true (double apply), don't care...
		}
		else
		{
			assert(it->active); // otherwise we have refresh/remove after remove, so missing apply in the middle of the fight?..
			it->active = !aura.isRemove;
		}
	}
	return res;
}

struct APMod
{
	int delta = 0;
	int potentialDelta = 0;
	int source = 0;
	int stackKey = 0;
	int expireTS = 0;
};

struct APMods
{
	std::vector<APMod> active;
	int valueMin = 320;
	int valueMax = 320;
	int validUntilTS = INT_MAX;
	bool multipleUnstackable = false;

	bool modify(const Event::AuraChange& event, int timestamp, const CombatantInfo& combatants)
	{
		struct AbilityData
		{
			int stackKey = 0;
			int delta = 0;
			int duration = 0;
			int improvingTalent = 3;
			int minPoints = 0;
			int maxPoints = 0;
			float pointEffect = 0.f;
		};
		static const auto kAbilities = []() {
			std::unordered_map<int, AbilityData> res;
			// curse of recklessness
			res[704]   = { 1, 20,  120000 };
			res[7658]  = { 1, 45,  120000 };
			res[7659]  = { 1, 65,  120000 };
			res[11717] = { 1, 90,  120000 };
			res[27226] = { 1, 135, 120000 };
			// screech
			res[24423] = { 2, -25,  4000 };
			res[24577] = { 2, -50,  4000 };
			res[24578] = { 2, -75,  4000 };
			res[24579] = { 2, -100, 4000 };
			res[27051] = { 2, -210, 4000 };
			// curse of weakness
			res[702]   = { 0, -21,  120000, 0, 5, 2, 0.1f };
			res[1108]  = { 0, -41,  120000, 0, 5, 2, 0.1f };
			res[6205]  = { 0, -64,  120000, 0, 5, 2, 0.1f };
			res[7646]  = { 0, -82,  120000, 0, 5, 2, 0.1f };
			res[11707] = { 0, -123, 120000, 0, 5, 2, 0.1f };
			res[11708] = { 0, -163, 120000, 0, 5, 2, 0.1f };
			res[27224] = { 0, -257, 120000, 0, 5, 2, 0.1f };
			res[30909] = { 0, -350, 120000, 0, 5, 2, 0.1f };
			// demoralizing shout
			res[1160]  = { 0, -45,  30000, 1, 5, 5, 0.08f };
			res[6190] =  { 0, -65,  30000, 1, 5, 5, 0.08f };
			res[11554] = { 0, -80,  30000, 1, 5, 5, 0.08f };
			res[11555] = { 0, -115, 30000, 1, 5, 5, 0.08f };
			res[11556] = { 0, -150, 30000, 1, 5, 5, 0.08f };
			res[25202] = { 0, -228, 30000, 1, 5, 5, 0.08f };
			res[25203] = { 0, -300, 30000, 1, 5, 5, 0.08f };
			// demoralizing roar
			res[99]    = { 0, -40,  30000, 1, 0, 5, 0.08f };
			res[1735]  = { 0, -60,  30000, 1, 0, 5, 0.08f };
			res[9490]  = { 0, -75,  30000, 1, 0, 5, 0.08f };
			res[9747]  = { 0, -110, 30000, 1, 0, 5, 0.08f };
			res[9898]  = { 0, -140, 30000, 1, 0, 5, 0.08f };
			res[26998] = { 0, -248, 30000, 1, 0, 5, 0.08f };
			return res;
		}();
		auto i = kAbilities.find(event.abilityGameID);
		if (i == kAbilities.end())
			return false; // irrelevant

		assert(event.isDebuff);
		assert(event.stacks == 0);
		auto proj = [](const APMod& m) { return std::make_pair(m.source, m.stackKey); };
		auto it = std::ranges::find(active, std::make_pair(event.source.actorID, i->second.stackKey), proj);
		if (event.isApply)
		{
			if (it == active.end())
			{
				int potentialDelta = 0;
				if (i->second.maxPoints > 0)
				{
					int curPoints = 61; // if combatant is unknown, assume worst
					if (auto iCombatant = combatants.find(event.source.actorID); iCombatant != combatants.end())
					{
						curPoints = iCombatant->second[i->second.improvingTalent];
					}
					curPoints = std::clamp(curPoints - i->second.minPoints, 0, i->second.maxPoints);
					potentialDelta = static_cast<int>(i->second.delta * i->second.pointEffect * curPoints);
				}
				active.push_back({ i->second.delta, potentialDelta, event.source.actorID, i->second.stackKey, timestamp + i->second.duration });
			}
			else
			{
				// double apply, just update expiration timestamp
				it->expireTS = timestamp + i->second.duration;
			}
		}
		else if (event.isRemove)
		{
			// TODO: damage events that will happen later in the same tick still benefit from removed aura...
			assert(it != active.end() && std::ranges::find(it + 1, active.end(), std::make_pair(event.source.actorID, i->second.stackKey), proj) == active.end());
			active.erase(it);
		}
		else
		{
			assert(it != active.end() && std::ranges::find(it + 1, active.end(), std::make_pair(event.source.actorID, i->second.stackKey), proj) == active.end());
			it->expireTS = timestamp + i->second.duration;
		}
		refresh();
		return true;
	}

	void refresh()
	{
		multipleUnstackable = false;
		validUntilTS = INT_MAX;
		int stackMin[3] = {}, stackMax[3] = {};
		for (auto& mod : active)
		{
			validUntilTS = std::min(validUntilTS, mod.expireTS);

			int curMin = mod.delta > 0 ? mod.delta : mod.delta + mod.potentialDelta;
			int curMax = mod.delta > 0 ? mod.delta + mod.potentialDelta : mod.delta;

			if (stackMin[mod.stackKey] != 0)
			{
				assert(stackMin[mod.stackKey] * mod.delta > 0);
				stackMin[mod.stackKey] = std::min(stackMin[mod.stackKey], curMin);
				stackMax[mod.stackKey] = std::max(stackMax[mod.stackKey], curMax);
				multipleUnstackable = true;
			}
			else
			{
				stackMin[mod.stackKey] = curMin;
				stackMax[mod.stackKey] = curMax;
			}
		}

		valueMin = valueMax = 320;
		for (int v : stackMin)
			valueMin += v;
		for (int v : stackMax)
			valueMax += v;
		valueMin = std::max(0, valueMin);
		valueMax = std::max(0, valueMax);
	}
};
#endif

//auto xBuffs = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:Buffs");
//auto xDebuffs = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:Debuffs");
//auto xCasts = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:Casts");
//auto xDamageDone = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageDone");
//auto xDamageTaken = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageTaken");
//auto xDTF = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageTaken hostilityType:Friendlies");
//auto xDDE = conn.fetchEvents("KxpRth4k6GBZqzPN", 2994927, 3552548, "dataType:DamageDone hostilityType:Enemies");

int main()
{
	//int dmgMin = 0, dmgMax = INT_MAX;
	////int dmgMin = 5770, dmgMax = 5770;
	//int numDmgEvents = 0, numOutliers = 0, numSuspectAP = 0, numSuspectAuraChange = 0;

	SwingAnalysis anal;
	anal.configNPC(21216, -16, 73, 600000); // hydross
	anal.configNPC(22035, -16, 71); // pure spawn of hydross
	anal.configNPC(22036, -8, 71); // tainted spawn of hydross

	WCLFetcher conn;
	auto sscTkReports = conn.fetchReportsForZone(1010);
	int processedReports = 0;
	for (auto& code : sscTkReports)
	{
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

		// process hydross fights (encounter id = 623)
		//auto findActorIDByGameID = [&](int gameID) {
		//	auto proj = [](const ReportActor& actor) { return actor.gameID; };
		//	auto it = std::ranges::find(report.actors, gameID, proj);
		//	if (it == report.actors.end())
		//		return -1;
		//	assert(std::ranges::find(it + 1, report.actors.end(), gameID, proj) == report.actors.end());
		//	return static_cast<int>(it - report.actors.begin());
		//};
		//auto hydrossActorID = findActorIDByGameID(21216);
		//if (hydrossActorID < 0)
		//{
		//	std::cout << "-> has hydross fight, but no hydross actor, skipping" << std::endl;
		//	continue;
		//}

		auto processHydrossFight = [&](const ReportFight& fight) {
			auto combatants = conn.fetchCombatantInfo(code, fight);
			if (!combatants)
			{
				std::cout << "-> fight " << fight.id << " has invalid combatants, skipping" << std::endl;
				return;
			}
			anal.analyzeFight(conn, report, fight, *combatants);

#if 0
			// workaround: sometimes log reports aura fade then damage in the same tick, but apparently aura still applies to this damage
			// note that there are other cases where in similar situation damage is correct
			// so just avoid damage events that happen in the same tick after aura change
			// note: not sure whether similar reordering problem can happen for aura apply before damage and damage before aura change, for now assume no...
			int prevEventTimestamp = 0;
			bool apChangedThisTick = false;

			auto events = conn.fetchEvents(code, fight.startTime, fight.endTime, "sourceID:" + std::to_string(hydrossActorID));
			auto missingAuras = detectMissingInitialAuras(events, hydrossActorID);
			APMods apState;
			for (auto& aura : missingAuras)
			{
				if (aura.isDebuff)
				{
					apState.modify(aura, fight.startTime, *combatants);
				}
			}
			for (auto& event : events)
			{
				if (event.timestamp != prevEventTimestamp)
				{
					prevEventTimestamp = event.timestamp;
					apChangedThisTick = false;
				}

				if (std::holds_alternative<Event::Damage>(event.data))
				{
					auto& dmg = std::get<Event::Damage>(event.data);
					if (dmg.source.actorID == hydrossActorID && dmg.abilityGameID == -16 && dmg.amountUnmit != 0)
					{
						if (event.timestamp - fight.startTime >= 600000)
							continue; // ignore enrage hits

						if (event.timestamp > apState.validUntilTS)
						{
							++numSuspectAP;
							continue;
						}

						if (apChangedThisTick)
						{
							++numSuspectAuraChange;
							continue;
						}

						if (dmg.hitType == Event::Crit)
							continue; // apparently resilience reduces unmitigated damage, so just skip crits to avoid outliers - there are few anyway

						++numDmgEvents;
						// note: we assume that damageUnmit = round(base * coeff), so we add +- 0.5f to cover rounding
						float divMin = 1.0f + apState.valueMin / 1540.1f;
						float divMax = 1.5f + apState.valueMax / 1540.1f;
						int curMin = static_cast<int>((dmg.amountUnmit - 0.5f) / divMax);
						int curMax = static_cast<int>((dmg.amountUnmit + 0.5f) / divMin);

						if (curMin > dmgMax)
						{
							std::cout << std::format("- outlier: report {}, fight {}, time {} ({}): range so far [{}-{}], new min {}", code, fight.id, (event.timestamp - fight.startTime) * 0.001f, event.timestamp, dmgMin, dmgMax, curMin) << std::endl;
							++numOutliers;
						}
						else if (curMin > dmgMin)
						{
							dmgMin = curMin;
						}

						if (curMax < dmgMin)
						{
							std::cout << std::format("- outlier: report {}, fight {}, time {} ({}): range so far [{}-{}], new max {}", code, fight.id, (event.timestamp - fight.startTime) * 0.001f, event.timestamp, dmgMin, dmgMax, curMax) << std::endl;
							++numOutliers;
						}
						else if (curMax < dmgMax)
						{
							dmgMax = curMax;
						}
					}
				}
				else if (std::holds_alternative<Event::AuraChange>(event.data))
				{
					auto& aura = std::get<Event::AuraChange>(event.data);
					if (aura.target.actorID == hydrossActorID && aura.isDebuff)
					{
						if (apState.modify(aura, event.timestamp, *combatants))
							apChangedThisTick |= aura.isRemove; // TODO: consider also setting this flag for isApply...
					}
				}
			}
#endif
		};
		for (auto& fight : report.fights)
			if (fight.encounterID == 623)
				processHydrossFight(fight);

		++processedReports;
	}

	//std::cout << std::format("Calculated range: [{}-{}], based on {} events ({} outliers, {} skipped due to suspect AP, {} skipped due to AP change in same tick)", dmgMin, dmgMax, numDmgEvents, numOutliers, numSuspectAP, numSuspectAuraChange) << std::endl;
	for (auto& [npc, data] : anal.data())
	{
		std::cout << std::format("{}: calculated range: [{}-{}] ({} crushes seen), based on {} events ({} outliers, {} crits, {} skipped due to suspect AP, {} skipped due to AP change in same tick)",
			npc, data.damageMin, data.damageMax, data.numSeenCrushes, data.numProcessedEvents, data.numOutliers, data.numIgnoredCrits, data.numSuspectAuraExpired, data.numSuspectAuraChange) << std::endl;
	}

	//auto rateLimit = conn.query("{rateLimitData{limitPerHour pointsSpentThisHour pointsResetIn}}", false);
	return 1;
}
