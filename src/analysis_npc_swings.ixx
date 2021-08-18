module;

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <map>
#include <variant>

// this module contains utilities to analyze NPC melee swing properties (damage, swing time, parry-haste, crushability)
export module analysis.npc_swings;
export import analysis.npc_ap;

export {
	class SwingAnalysis
	{
		struct AnalysisActorState
		{
			const NPCAPHistory::APTrack& track;
			NPCAPHistory::APTrack::const_iterator apIter;

			AnalysisActorState(const NPCAPHistory& history, const EventActor& actor) : track(history.get(actor)), apIter(track.begin()) {}
		};

	public:
		struct NPCInfo
		{
			int swingAbilityID = 0;
			int baseAP = 0;
			float factorAP = 0.f;
			int timeCutoff = 0;
			int damageMin = 0;
			int damageMax = 0;
			int numSeenCrushes = 0;
			int numProcessedEvents = 0;
			int numOutliers = 0;
			int numIgnoredCrits = 0;
			int numSuspectAuraExpired = 0;
			int numSuspectAuraChange = 0;
		};

		// configuration: add NPC to analyze
		// events with timestamps greater than fight-start + cutoff are ignored (used to ignore e.g. enrage swings)
		void configNPC(int gameID, int swingAbilityID, int level, int timeCutoff = INT_MAX, int initialDamageMin = 0, int initialDamageMax = INT_MAX)
		{
			auto& data = mData[gameID];
			data.swingAbilityID = swingAbilityID;
			data.baseAP = std::lround(level * 5.5f - 81.8f);
			data.factorAP = level * 26.7f - 409.f; // 5 * (Level * 5.34 - 81.8)
			data.timeCutoff = timeCutoff;
			data.damageMin = initialDamageMin;
			data.damageMax = initialDamageMax;
		}

		// analyze data from specified fight
		void analyzeFight(WCLFetcher& conn, const Report& report, const ReportFight& fight, const CombatantInfo& combatants)
		{
			std::map<EventActor, AnalysisActorState> states;
			NPCAPHistory ap{ conn, report.code, fight, combatants };
			auto events = conn.fetchEvents(report.code, fight.startTime, fight.endTime, "dataType:DamageDone hostilityType:Enemies");
			for (auto& event : events)
			{
				auto* dmg = std::get_if<Event::Damage>(&event.data);
				if (!dmg)
					continue;

				auto actorGameID = report.actors[dmg->source.actorID != -1 ? dmg->source.actorID : 0].gameID;
				auto iData = mData.find(actorGameID);
				if (iData == mData.end())
					continue; // don't care about this actor
				auto& data = iData->second;

				if (dmg->abilityGameID != data.swingAbilityID)
					continue; // don't care about this ability

				if (dmg->hitType == Event::Crush)
					++data.numSeenCrushes;

				if (data.timeCutoff <= event.timestamp - fight.startTime)
					continue; // ignore swings after cutoff

				if (dmg->amountUnmit == 0)
					continue; // ignore fully avoided swings (note: this is valid only for swing damage calculation)

				if (dmg->hitType == Event::Crit)
				{
					// apparently resilience reduces unmitigated damage, so just skip crits to avoid outliers - there are few anyway
					++data.numIgnoredCrits;
					continue;
				}

				auto& state = states.try_emplace(dmg->source, ap, dmg->source).first->second;
				while (state.apIter->endTimestamp < event.timestamp)
					++state.apIter;

				if (state.apIter->endTimestamp == event.timestamp)
				{
					// if we have aura change and damage events with same timestamp, we don't actually know whether this aura applies to damage
					// for example, i've seen logs with aura fade followed by damage, and damage still benefitted from aura
					// so just ignore such events, they are relatively rare...
					++data.numSuspectAuraChange;
					continue;
				}

				if (state.apIter->haveExpiredAuras)
				{
					// sometimes logs miss aura-remove event, we try to detect it conservatively and ignore such damage events
					// there are false positives (e.g. warrior could have talent that increases DS duration, or we might get fade event few ms later), we just ignore such events too
					++data.numSuspectAuraExpired;
					continue;
				}
	
				// note: we assume that damageUnmit = round(base * coeff), so we add +- 0.5f to cover rounding
				int apMin = std::clamp(data.baseAP + state.apIter->apModMin, 0, INT_MAX);
				int apMax = std::clamp(data.baseAP + state.apIter->apModMax, 0, INT_MAX);
				float coeffMin = 1.0f + apMin / data.factorAP;
				float coeffMax = 1.5f + apMax / data.factorAP;
				int curMin = static_cast<int>((dmg->amountUnmit - 0.5f) / coeffMax);
				int curMax = static_cast<int>((dmg->amountUnmit + 0.5f) / coeffMin);
				if (curMin > data.damageMax || curMax < data.damageMin)
				{
					// current range does not intersect with range gathered so far, report an outlier and ignore
					std::cout << std::format("- outlier: report {}, fight {}, time {} ({}): range so far [{}-{}], new range [{}-{}]",
						report.code, fight.id, (event.timestamp - fight.startTime) * 0.001f, event.timestamp, data.damageMin, data.damageMax, curMin, curMax) << std::endl;
					++data.numOutliers;
					continue;
				}

				++data.numProcessedEvents;
				if (curMin > data.damageMin)
					data.damageMin = curMin;
				if (curMax < data.damageMax)
					data.damageMax = curMax;
			}
		}

		// accessors
		const auto& data() const { return mData; }

	private:
		std::map<int, NPCInfo> mData;
	};
}
