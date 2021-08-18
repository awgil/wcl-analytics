module;

#include <cassert>
#include <format>
#include <map>
#include <variant>
#include <vector>

// this module contains utilities to analyze auras on actors
export module analysis.auras;
export import wcl.fetcher;

// TODO: unfortunately there's a problem with unordered_map<EventActor, ...> and std::hash<EventActor> specializations in another module in current MSVC implementation...

export {
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
					if (aura->isApply)
					{
						if (track.back().remove)
						{
							track.back().remove = false; // convert remove to refresh and pulse (apply+remove+apply) to apply
							track.back().implicit = true;
						}
						// else: refresh+apply or apply+apply are left as-is
					}
					else if (aura->isRemove)
					{
						if (!track.back().remove)
						{
							track.back().remove = true; // convert apply to pulse and refresh to remove
							track.back().implicit = true;
						}
						// remove/pulse -> remove is double remove, ignore...
					}
					else
					{
						assert(!track.back().remove); // ??? remove/pulse + refresh, never seen one...
						// apply/refresh + refresh - just ignore second refresh
					}
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

		// TODO: merge API to add other combination of buffs/debuffs or enemies/friends

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
}
