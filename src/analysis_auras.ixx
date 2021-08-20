module;

#include <algorithm>
#include <cassert>
#include <format>
#include <map>
#include <numeric>
#include <set>
#include <variant>
#include <vector>

// this module contains utilities to analyze auras on actors
export module analysis.auras;
export import wcl.fetcher;

// TODO: unfortunately there's a problem with unordered_map<EventActor, ...> and std::hash<EventActor> specializations in another module in current MSVC implementation...

export {
	// full set of auras (buffs/debuffs) on multiple targets; filtered and sanitized (logs sometimes create weird effects, this is fixed to the best effort)
	class Auras
	{
	public:
		// aura change event
		// note: sometimes aura is applied and removed in the same tick - we consider it a 'pulse' (event with apply + remove)
		struct Edge
		{
			int timestamp = 0;
			bool apply = false; // true for apply and pulse, false for remove or refresh (== was it inactive before?)
			bool remove = false; // true for remove and pulse, false for apply or refresh (== is it inactive after?)
			bool implicit = false; // true if this event is not available in log and is inferred (e.g. apply on fight start if first event is remove, refresh if apply is followed by apply, remove if last event is not a remove)
			// TODO: stacks...
		};

		// history of a single aura from a single source on a single target; sorted by timestamp
		// first event is always apply, last is always remove, apply is always followed by refresh or remove, remove is always preceeded by apply or refresh
		using Track = std::vector<Edge>;

		// history of a single aura for a single actor in a single fight
		using TracksBySource = std::map<EventActor, Track>;

		// history of auras for a single actor in a single fight
		using ActorHistory = std::map<int, TracksBySource>;

		// analyze provided events to build a set of active auras; start/end timestamps is used to create implicit edges
		Auras(const std::vector<Event>& events, int implicitStartTS, int implicitEndTS)
		{
			for (auto& event : events)
			{
				auto* aura = std::get_if<Event::AuraChange>(&event.data);
				if (!aura)
					continue;

				if (aura->stacks != 0)
					continue; // TODO: handle stacks

				auto& track = mData[aura->target][aura->abilityGameID][aura->source];
				if (!track.empty() && track.back().timestamp == event.timestamp)
				{
					// handle double change at the same time
					if (track.back().remove != aura->isRemove)
					{
						// remove/pulse -> apply/refresh -or- add/refresh -> remove
						track.back().remove = aura->isRemove;
						track.back().implicit = true;
					}
					// else: remove/pulse -> remove -or- add/refresh -> apply/refresh - just ignore second event
					continue;
				}

				assert(track.empty() || track.back().timestamp < event.timestamp); // whatever we do, maintain sort invariant
				if (aura->isApply)
				{
					if (track.empty() || track.back().remove)
					{
						// normal apply: aura is not active
						track.push_back({ event.timestamp, true });
					}
					else
					{
						// apply after apply/refresh: this is not normal, convert into implicit refresh
						track.push_back({ event.timestamp, false, false, true });
					}
				}
				else
				{
					if (!track.empty())
					{
						// note: sometimes we get refresh/remove after remove, this is weird :( convert into implicit apply/pulse, that's not ideal though...
						track.push_back({ event.timestamp, track.back().remove, aura->isRemove, track.back().remove });
					}
					else if (implicitStartTS < event.timestamp)
					{
						// remove/refresh as a first event - create implicit apply
						track.push_back({ implicitStartTS, true, false, true });
						track.push_back({ event.timestamp, false, aura->isRemove, false });
					}
					else
					{
						assert(implicitStartTS == event.timestamp);
						// remove/refresh right at fight start - create a single apply/pulse event
						track.push_back({ event.timestamp, true, aura->isRemove, true });
					}
				}
			}

			// finish tracks that aren't finished explicitly
			for (auto& [target, history] : mData)
			{
				for (auto& [id, tracks] : history)
				{
					for (auto& [source, track] : tracks)
					{
						assert(!track.empty());
						auto& last = track.back();
						if (!last.remove)
						{
							if (last.timestamp < implicitEndTS)
							{
								track.push_back({ implicitEndTS, false, true, true });
							}
							else if (last.timestamp == implicitEndTS)
							{
								last.remove = true;
								last.implicit = true;
							}
							else
							{
								assert(false); // ts > end???
							}
						}
					}
				}
			}
		}

		// fetch events and analyze
		Auras(WCLFetcher& conn, const std::string& code, const ReportFight& fight, bool debuffs, bool enemies)
			: Auras(conn.fetchEvents(code, fight.startTime, fight.endTime, std::format("dataType:{} hostilityType:{}", debuffs ? "Debuffs" : "Buffs", enemies ? "Enemies" : "Friendlies")), fight.startTime, fight.endTime)
		{
		}

		// accessors
		const auto& data() const { return mData; }
		auto begin() const { return mData.begin(); }
		auto end() const { return mData.end(); }

		const auto& on(const EventActor& target) const
		{
			static const ActorHistory kEmpty;
			auto it = mData.find(target);
			return it != mData.end() ? it->second : kEmpty;
		}

		const auto& history(const EventActor& target, int abilityGameID) const
		{
			static const TracksBySource kEmpty;
			auto& entry = on(target);
			auto it = entry.find(abilityGameID);
			return it != entry.end() ? it->second : kEmpty;
		}

		const auto& track(const EventActor& target, int abilityGameID, const EventActor& source) const
		{
			static const Track kEmpty;
			auto& entry = history(target, abilityGameID);
			auto it = entry.find(source);
			return it != entry.end() ? it->second : kEmpty;
		}

	private:
		std::map<EventActor, ActorHistory> mData; // key = aura target
	};

	// history of parameter values due to aura changes
	// it is assumed that several auras can influence same parameter - some stacking, some not
	// some auras can have their effect improved by talents - unfortunately, we don't know exact talents, so we try to build ranges of possible values
	// sometimes logs don't contain fade event, so we try to detect time ranges with potentially expired auras - further analysis can treat these ranges as more suspect
	class AuraEffectHistory
	{
	public:
		static constexpr int kMaxStackKeys = 3; // increase as needed (used to allocate storage on stack on hot path)

		// description of a single aura effect
		struct AuraEffect
		{
			int abilityGameID = 0;
			int stackKey = 0; // effects with same stack key do not stack
			int effect = 0; // effect value
			int duration = 0; // 0 means 'undefined'; TODO: handle talents that increase duration?..
			int improveMaxPoints = 0; // if >0, there is a talent with up to this amount of points that increases effect
			int improveMinPoints = 0; // min points in tree to get this talent
			int improveTalentTree = 3; // index of the talent tree containing this talent
			float improvePointEffect = 0.f; // extra talent effect = num points * this value * base effect
		};
		using AuraEffects = std::vector<AuraEffect>; // assume all ability IDs are unique

		// effect state, from the end of the previous state to the specified timestamp
		struct EffectState
		{
			int endTimestamp = 0;
			int rangeMin = 0;
			int rangeMax = 0;
			bool haveExpiredAuras = false;
			bool haveMultipleUnstackable = false;
		};
		// history for a single actor; sorted by timestamps; never empty, last entry has timestamp==INT_MAX
		using EffectTrack = std::vector<EffectState>;

		AuraEffectHistory(const Auras& auras, const CombatantInfo& combatants, const AuraEffects& effects)
		{
			// sanity checks
			assert(std::ranges::all_of(effects, [](const AuraEffect& eff) { return eff.stackKey >= 0 && eff.stackKey < kMaxStackKeys && (eff.improveMaxPoints == 0 || (eff.improveTalentTree >= 0 && eff.improveTalentTree < 3)); }));
			for (auto& [actor, history] : auras.data())
			{
				mTracks[actor] = buildTrack(history, combatants, effects);
			}
		}

		// accessors
		const auto& get(const EventActor& actor) const
		{
			static const EffectTrack kEmpty{ EffectState{ INT_MAX } };
			auto it = mTracks.find(actor);
			return it != mTracks.end() ? it->second : kEmpty;
		}

	private:
		struct AuraEffectInstance
		{
			int stackKey = 0;
			int rangeMin = 0;
			int rangeMax = 0;
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
			std::vector<AuraEffectInstance> props;
			std::vector<AuraEvent> events;

			void add(const Auras::ActorHistory& auras, const CombatantInfo& combatants, const AuraEffect& effect)
			{
				auto iAura = auras.find(effect.abilityGameID);
				if (iAura == auras.end())
					return; // this aura is never applied to the actor

				for (auto& [source, track] : iAura->second)
				{
					int rangeMin = effect.effect, rangeMax = effect.effect;
					if (effect.improveMaxPoints > 0)
					{
						int curPoints = 61; // if combatant is unknown, assume the worst
						if (auto iCombatant = combatants.find(source.actorID); iCombatant != combatants.end())
						{
							curPoints = iCombatant->second[effect.improveTalentTree];
						}
						curPoints = std::clamp(curPoints - effect.improveMinPoints, 0, effect.improveMaxPoints);
						auto improvement = static_cast<int>(effect.effect * effect.improvePointEffect * curPoints);
						(improvement > 0 ? rangeMax : rangeMin) += improvement;
					}

					auto index = props.size();
					props.push_back({ effect.stackKey, rangeMin, rangeMax });

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
								activeExpireTS = effect.duration > 0 ? edge.timestamp + effect.duration : INT_MAX;
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
							activeExpireTS = effect.duration > 0 ? edge.timestamp + effect.duration : INT_MAX;
						}
					}
				}
			}
		};

		EffectTrack buildTrack(const Auras::ActorHistory& auras, const CombatantInfo& combatants, const AuraEffects& effects)
		{
			ActorCalculator calc;
			for (auto& effect : effects)
				calc.add(auras, combatants, effect);

			// sort events by timestamp
			std::ranges::sort(calc.events, {}, [](const AuraEvent& e) { return e.timestamp; });

			// build track by going through aura changes in order
			// note that we postpone refresh, since we could have multiple changes at the same time
			std::set<size_t> activeAuras;
			int numExpired = 0;
			int prevTS = 0;
			EffectTrack res;

			auto commit = [&](int endTS) {
				bool multipleUnstackable = false;
				int stackMin[kMaxStackKeys] = {}, stackMax[kMaxStackKeys] = {};
				for (size_t index : activeAuras)
				{
					auto& props = calc.props[index];
					if (stackMin[props.stackKey] != 0)
					{
						assert(stackMin[props.stackKey] * props.rangeMin > 0);
						stackMin[props.stackKey] = std::min(stackMin[props.stackKey], props.rangeMin);
						stackMax[props.stackKey] = std::max(stackMax[props.stackKey], props.rangeMax);
						multipleUnstackable = true;
					}
					else
					{
						stackMin[props.stackKey] = props.rangeMin;
						stackMax[props.stackKey] = props.rangeMax;
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
		std::map<EventActor, EffectTrack> mTracks;
	};
}
