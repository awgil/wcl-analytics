module;

#include <algorithm>
#include <cassert>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

// this module contains utilities to analyze NPC AP history based on applied debuffs
export module analysis.npc_ap;
export import analysis.auras;

export {
	class NPCAPHistory
	{
	public:
		// history for a single actor (track sorted by timestamps); never empty, last entry has timestamp==INT_MAX
		struct APState
		{
			int endTimestamp = 0;
			int apModMin = 0;
			int apModMax = 0;
			bool haveExpiredAuras = false;
			bool haveMultipleUnstackable = false;
		};
		using APTrack = std::vector<APState>;

		NPCAPHistory(WCLFetcher& conn, const std::string& code, const ReportFight& fight, const CombatantInfo& combatants)
		{
			Auras auras{ conn, code, fight, true, true };
			// TODO: try to detect DS duration talent...
			for (auto& [actor, history] : auras.data())
			{
				mTracks[actor] = buildTrack(history, combatants);
			}
		}

		// accessors
		const auto& get(const EventActor& actor) const
		{
			static const APTrack kEmpty{ APState{ INT_MAX } };
			auto it = mTracks.find(actor);
			return it != mTracks.end() ? it->second : kEmpty;
		}

	private:
		struct AuraProperties
		{
			int stackKey = 0;
			int apMod = 0;
			int apModPotential = 0;
		};

		enum EventType { Apply, Remove, Pulse, Expire, Unexpire, RemoveExpired };

		struct AuraEvent
		{
			int timestamp;
			EventType event;
			size_t index;
		};

		struct ActorCalculator
		{
			std::vector<AuraProperties> props;
			std::vector<AuraEvent> events;

			void add(const Auras::ActorHistory& auras, const CombatantInfo& combatants, int abilityID, int stackKey, int apMod, int duration, int improveMaxPoints = 0, int improveMinPoints = 0, int improveTalentTree = 3, float improvePointEffect = 0.f)
			{
				auto iAura = auras.find(abilityID);
				if (iAura == auras.end())
					return; // this aura is never applied to the actor

				for (auto& [source, track] : iAura->second)
				{
					int potential = 0;
					if (improveMaxPoints > 0)
					{
						int curPoints = 61; // if combatant is unknown, assume worst
						if (auto iCombatant = combatants.find(source.actorID); iCombatant != combatants.end())
						{
							curPoints = iCombatant->second[improveTalentTree];
						}
						curPoints = std::clamp(curPoints - improveMinPoints, 0, improveMaxPoints);
						potential = static_cast<int>(apMod * improvePointEffect * curPoints);
					}

					auto index = props.size();
					props.push_back({ stackKey, apMod, potential });

					int activeExpireTS = 0;
					for (auto& edge : track)
					{
						if (edge.apply)
						{
							assert(events.empty() || events.back().index != index || events.back().event == Remove || events.back().event == Pulse || events.back().event == RemoveExpired);
							assert(activeExpireTS == 0);
							if (edge.remove)
							{
								events.push_back({ edge.timestamp, Pulse, index });
							}
							else
							{
								events.push_back({ edge.timestamp, Apply, index });
								activeExpireTS = edge.timestamp + duration;
							}
							continue;
						}

						// refresh or remove - first check whether aura expired before this event
						assert(!events.empty() && events.back().index == index && events.back().event != Remove && events.back().event != Pulse && events.back().event != Expire && events.back().event != RemoveExpired);
						assert(activeExpireTS != 0);
						bool currentlyExpired = false;
						if (activeExpireTS < edge.timestamp)
						{
							events.push_back({ activeExpireTS, Expire, index });
							currentlyExpired = true;
						}

						if (edge.remove)
						{
							events.push_back({ edge.timestamp, currentlyExpired ? RemoveExpired : Remove, index });
							activeExpireTS = 0;
						}
						else
						{
							if (currentlyExpired)
							{
								events.push_back({ edge.timestamp, Unexpire, index });
							}
							activeExpireTS = edge.timestamp + duration;
						}
					}
				}
			}
		};

		APTrack buildTrack(const Auras::ActorHistory& auras, const CombatantInfo& combatants)
		{
			ActorCalculator calc;
			// demoralizing shout
			calc.add(auras, combatants, 1160,  0, -45,  30000, 5, 5, 1, 0.08f);
			calc.add(auras, combatants, 6190,  0, -65,  30000, 5, 5, 1, 0.08f);
			calc.add(auras, combatants, 11554, 0, -80,  30000, 5, 5, 1, 0.08f);
			calc.add(auras, combatants, 11555, 0, -115, 30000, 5, 5, 1, 0.08f);
			calc.add(auras, combatants, 11556, 0, -150, 30000, 5, 5, 1, 0.08f);
			calc.add(auras, combatants, 25202, 0, -228, 30000, 5, 5, 1, 0.08f);
			calc.add(auras, combatants, 25203, 0, -300, 30000, 5, 5, 1, 0.08f);
			// demoralizing roar
			calc.add(auras, combatants, 99,    0, -40,  30000, 5, 0, 1, 0.08f);
			calc.add(auras, combatants, 1735,  0, -60,  30000, 5, 0, 1, 0.08f);
			calc.add(auras, combatants, 9490,  0, -75,  30000, 5, 0, 1, 0.08f);
			calc.add(auras, combatants, 9747,  0, -110, 30000, 5, 0, 1, 0.08f);
			calc.add(auras, combatants, 9898,  0, -140, 30000, 5, 0, 1, 0.08f);
			calc.add(auras, combatants, 26998, 0, -248, 30000, 5, 0, 1, 0.08f);
			// curse of weakness
			calc.add(auras, combatants, 702,   0, -21,  120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 1108,  0, -41,  120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 6205,  0, -64,  120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 7646,  0, -82,  120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 11707, 0, -123, 120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 11708, 0, -163, 120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 27224, 0, -257, 120000, 2, 5, 0, 0.1f);
			calc.add(auras, combatants, 30909, 0, -350, 120000, 2, 5, 0, 0.1f);
			// curse of recklessness
			calc.add(auras, combatants, 704,   1, +20,  120000);
			calc.add(auras, combatants, 7658,  1, +45,  120000);
			calc.add(auras, combatants, 7659,  1, +65,  120000);
			calc.add(auras, combatants, 11717, 1, +90,  120000);
			calc.add(auras, combatants, 27226, 1, +135, 120000);
			// screech
			calc.add(auras, combatants, 24423, 2, -25,  4000);
			calc.add(auras, combatants, 24577, 2, -50,  4000);
			calc.add(auras, combatants, 24578, 2, -75,  4000);
			calc.add(auras, combatants, 24579, 2, -100, 4000);
			calc.add(auras, combatants, 27051, 2, -210, 4000);

			// sort events by timestamp
			std::ranges::sort(calc.events, {}, [](const AuraEvent& e) { return e.timestamp; });

			// build track by going through aura changes in order
			// note that we postpone refresh, since we could have multiple changes at the same time
			std::set<size_t> activeAuras;
			int numExpired = 0;
			int prevTS = 0;
			APTrack res;

			auto commit = [&](int endTS) {
				bool multipleUnstackable = false;
				int stackMin[3] = {}, stackMax[3] = {};
				for (size_t index : activeAuras)
				{
					auto& props = calc.props[index];
					int curMin = props.apMod > 0 ? props.apMod : props.apMod + props.apModPotential;
					int curMax = props.apMod > 0 ? props.apMod + props.apModPotential : props.apMod;

					if (stackMin[props.stackKey] != 0)
					{
						assert(stackMin[props.stackKey] * props.apMod > 0);
						stackMin[props.stackKey] = std::min(stackMin[props.stackKey], curMin);
						stackMax[props.stackKey] = std::max(stackMax[props.stackKey], curMax);
						multipleUnstackable = true;
					}
					else
					{
						stackMin[props.stackKey] = curMin;
						stackMax[props.stackKey] = curMax;
					}
				}

				int min = std::accumulate(std::begin(stackMin), std::end(stackMin), 0);
				int max = std::accumulate(std::begin(stackMax), std::end(stackMax), 0);
				res.push_back({ endTS, min, max, numExpired > 0, multipleUnstackable });
			};

			for (auto& event : calc.events)
			{
				if (prevTS < event.timestamp)
				{
					commit(event.timestamp);
					prevTS = event.timestamp;
				}

				bool success = true;
				switch (event.event)
				{
				case Apply:
					success = activeAuras.insert(event.index).second;
					break;
				case Remove:
					success = activeAuras.erase(event.index) != 0;
					break;
				case Pulse:
					// do nothing, but force creation of a new segment, so that we ignore damage events during pulse tick
					break;
				case Expire:
					++numExpired;
					break;
				case Unexpire:
					--numExpired;
					break;
				case RemoveExpired:
					success = activeAuras.erase(event.index) != 0;
					--numExpired;
					break;
				}
				assert(success);
			}
			commit(INT_MAX);
			return res;
		}

	private:
		std::map<EventActor, APTrack> mTracks;
	};
}
