#pragma once
#include "csd_builder.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

// A drag is reconstructed from its starting snapshot on every frame, so moving
// back and forth or crossing tracks never moves a point twice.
struct ClipAutomationMove {
    struct TaggedPoint { AutoPoint point; int clipId = 0; };
    std::map<int, std::map<std::string, std::vector<TaggedPoint>>> lanes;
    std::map<int, std::map<std::string, CawParam>> ranges;

    void capture(const Project& project, const std::set<int>& selected) {
        lanes.clear();
        ranges.clear();
        for (const Track& track : project.tracks) {
            ranges[track.id] = track.automationRanges;
            std::map<int, std::set<std::string>> channels;
            for (const Clip& clip : track.clips) if (selected.count(clip.id))
                for (const CawParam& p : parseClip(clip).params) channels[clip.id].insert(p.chn);
            auto& snapshot = lanes[track.id];
            for (const auto& lane : track.automation) {
                auto& saved = snapshot[lane.first];
                for (const AutoPoint& point : lane.second) {
                    int owner = 0;
                    for (const Clip& clip : track.clips) {
                        if (selected.count(clip.id) && channels[clip.id].count(lane.first) &&
                            point.t >= clip.start - 1e-9 && point.t <= clip.start + clip.dur + 1e-9) {
                            owner = clip.id;
                            break;
                        }
                    }
                    saved.push_back({point, owner});
                }
            }
        }
    }

    void apply(Project& project, double dt) const {
        std::map<int, Track*> tracks;
        std::map<int, int> clipTracks;
        for (Track& track : project.tracks) {
            tracks[track.id] = &track;
            for (const Clip& clip : track.clips) clipTracks[clip.id] = track.id;
            track.automation.clear();
        }
        for (const auto& original : lanes) {
            if (!tracks.count(original.first)) continue;
            for (const auto& lane : original.second) {
                tracks[original.first]->automation[lane.first]; // retain empty lanes
                for (const TaggedPoint& tagged : lane.second) {
                    AutoPoint point = tagged.point;
                    int target = original.first;
                    if (tagged.clipId && clipTracks.count(tagged.clipId)) {
                        point.t = std::max(0.0, point.t + dt);
                        target = clipTracks[tagged.clipId];
                        const auto sourceRanges = ranges.find(original.first);
                        if (target != original.first && sourceRanges != ranges.end() && !tracks[target]->automationRanges.count(lane.first)) {
                            const auto range = sourceRanges->second.find(lane.first);
                            if (range != sourceRanges->second.end()) tracks[target]->automationRanges[lane.first] = range->second;
                        }
                    }
                    tracks[target]->automation[lane.first].push_back(point);
                }
            }
        }
        for (Track& track : project.tracks)
            for (auto& lane : track.automation)
                std::stable_sort(lane.second.begin(), lane.second.end(),
                    [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
    }
};
