module;

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <map>
#include <variant>

// this module contains utilities to analyze NPC melee swing properties (damage, swing time, parry-haste, crushability)
export module analysis.npc_swings;
export import analysis.auras;

export {
	class SwingAnalysis
	{
		struct AnalysisActorState
		{
			AuraEffectHistory::EffectTrack::const_iterator apIter;
			AuraEffectHistory::EffectTrack::const_iterator physDmgReduceIter;

			AnalysisActorState(const AuraEffectHistory& ap, const AuraEffectHistory& physDmgReduce, const EventActor& actor)
				: apIter(ap.get(actor).begin())
				, physDmgReduceIter(physDmgReduce.get(actor).begin())
			{
			}
		};

	public:
		struct NPCInfo
		{
			int swingAbilityID = 0;
			float baseAP = 0;
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
			data.baseAP = level * 5.5f - 81.8f;
			data.factorAP = level * 26.7f - 409.f; // 5 * (Level * 5.34 - 81.8)
			data.timeCutoff = timeCutoff;
			data.damageMin = initialDamageMin;
			data.damageMax = initialDamageMax;
		}

		// analyze data from specified fight
		void analyzeFight(WCLFetcher& conn, const Report& report, const ReportFight& fight, const CombatantInfo& combatants)
		{
			static const AuraEffectHistory::AuraEffects kAPMods = {
				// demoralizing shout (TODO: try to detect duration talent...)
				{ 1160,  0, -45,  30000, 5, 5, 1, 0.08f },
				{ 6190,  0, -65,  30000, 5, 5, 1, 0.08f },
				{ 11554, 0, -80,  30000, 5, 5, 1, 0.08f },
				{ 11555, 0, -115, 30000, 5, 5, 1, 0.08f },
				{ 11556, 0, -150, 30000, 5, 5, 1, 0.08f },
				{ 25202, 0, -228, 30000, 5, 5, 1, 0.08f },
				{ 25203, 0, -300, 30000, 5, 5, 1, 0.08f },
				// demoralizing roar
				{ 99,    0, -40,  30000, 5, 0, 1, 0.08f },
				{ 1735,  0, -60,  30000, 5, 0, 1, 0.08f },
				{ 9490,  0, -75,  30000, 5, 0, 1, 0.08f },
				{ 9747,  0, -110, 30000, 5, 0, 1, 0.08f },
				{ 9898,  0, -140, 30000, 5, 0, 1, 0.08f },
				{ 26998, 0, -248, 30000, 5, 0, 1, 0.08f },
				// curse of weakness
				{ 702,   0, -21,  120000, 2, 5, 0, 0.1f },
				{ 1108,  0, -41,  120000, 2, 5, 0, 0.1f },
				{ 6205,  0, -64,  120000, 2, 5, 0, 0.1f },
				{ 7646,  0, -82,  120000, 2, 5, 0, 0.1f },
				{ 11707, 0, -123, 120000, 2, 5, 0, 0.1f },
				{ 11708, 0, -163, 120000, 2, 5, 0, 0.1f },
				{ 27224, 0, -257, 120000, 2, 5, 0, 0.1f },
				{ 30909, 0, -350, 120000, 2, 5, 0, 0.1f },
				// curse of recklessness
				{ 704,   1, +20,  120000 },
				{ 7658,  1, +45,  120000 },
				{ 7659,  1, +65,  120000 },
				{ 11717, 1, +90,  120000 },
				{ 27226, 1, +135, 120000 },
				// screech
				{ 24423, 2, -25,  4000 },
				{ 24577, 2, -50,  4000 },
				{ 24578, 2, -75,  4000 },
				{ 24579, 2, -100, 4000 },
				{ 27051, 2, -210, 4000 },
			};
			static const AuraEffectHistory::AuraEffects kPhysDamageReductionMods = {
				// shadow embrace
				{ 32386, 0, 1 },
				{ 32388, 0, 2 },
				{ 32389, 0, 3 },
				{ 32390, 0, 4 },
				{ 32391, 0, 5 },
			};
			Auras enemyDebuffs{ conn, report.code, fight, true, true };
			AuraEffectHistory ap{ enemyDebuffs, combatants, kAPMods };
			AuraEffectHistory physReduction{ enemyDebuffs, combatants, kPhysDamageReductionMods };

			std::map<EventActor, AnalysisActorState> states;
			auto events = conn.fetchEvents(report.code, fight.startTime, fight.endTime, "dataType:DamageDone hostilityType:Enemies");
			for (auto& event : events)
			{
				auto* dmg = std::get_if<Event::Damage>(&event.data);
				if (!dmg)
					continue;

				auto& actor = report.actors[dmg->source.actorID != -1 ? dmg->source.actorID : 0];
				auto iData = mData.find(actor.gameID);
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

				auto& state = states.try_emplace(dmg->source, ap, physReduction, dmg->source).first->second;
				while (state.apIter->endTimestamp < event.timestamp)
					++state.apIter;
				while (state.physDmgReduceIter->endTimestamp < event.timestamp)
					++state.physDmgReduceIter;

				bool physical = data.swingAbilityID > 0; // TODO: this is a heuristic that is correct only for melee swings
				if (physical && state.physDmgReduceIter->rangeMin != state.physDmgReduceIter->rangeMax)
					continue; // multiple different ranks of shadow embrace - looks unrealistic...

				if (state.apIter->endTimestamp == event.timestamp || (physical && state.physDmgReduceIter->endTimestamp == event.timestamp))
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

				// formula: damageUnmit = base * (random[1, 1.5] + AP/factor) * (1 - reduction)
				// note: we assume that damageUnmit = round(base * coeff), so we add +- 0.5f to cover rounding
				float coeffReduction = physical ? 1.f - 0.01f * state.physDmgReduceIter->rangeMin : 1.f;
				float apMin = std::clamp(data.baseAP + state.apIter->rangeMin, 0.f, FLT_MAX);
				float apMax = std::clamp(data.baseAP + state.apIter->rangeMax, 0.f, FLT_MAX);
				float coeffMin = 1.0f + apMin / data.factorAP;
				float coeffMax = 1.5f + apMax / data.factorAP;
				int curMin = static_cast<int>((dmg->amountUnmit - 0.5f) / (coeffMax * coeffReduction));
				int curMax = static_cast<int>((dmg->amountUnmit + 0.5f) / (coeffMin * coeffReduction));
				if (curMin > data.damageMax || curMax < data.damageMin)
				{
					// current range does not intersect with range gathered so far, report an outlier and ignore
					std::cout << std::format("- outlier: report {}, fight {}, time {:.3f} ({}): target {}, range so far [{}-{}], unmit {}, apMod in [{}-{}], reduction {:.2f} => new range [{}-{}]",
						report.code, fight.id, (event.timestamp - fight.startTime) * 0.001f, event.timestamp, actor.name, data.damageMin, data.damageMax, dmg->amountUnmit, state.apIter->rangeMin, state.apIter->rangeMax, coeffReduction, curMin, curMax) << std::endl;
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
