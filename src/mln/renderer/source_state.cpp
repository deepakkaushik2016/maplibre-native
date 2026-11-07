#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/source_state.hpp>
#include <mln/style/conversion_impl.hpp>
#include <mln/util/instrumentation.hpp>
#include <mln/util/logging.hpp>

namespace mln {

void SourceFeatureState::updateState(const std::optional<std::string>& sourceLayerID,
                                     const std::string& featureID,
                                     const FeatureState& newState) {
    std::string sourceLayer = sourceLayerID.value_or(std::string());
    for (const auto& state : newState) {
        auto& layerStates = stateChanges[sourceLayer];
        auto& featureStates = layerStates[featureID];
        featureStates[state.first] = state.second;
    }
}

void SourceFeatureState::getState(FeatureState& result,
                                  const std::optional<std::string>& sourceLayerID,
                                  const std::string& featureID) const {
    std::string sourceLayer = sourceLayerID.value_or(std::string());
    FeatureState current;
    FeatureState changes;
    auto layerStates = currentStates.find(sourceLayer);
    if (layerStates != currentStates.end()) {
        const auto currentStateEntry = layerStates->second.find(featureID);
        if (currentStateEntry != layerStates->second.end()) {
            current = currentStateEntry->second;
        }
    }

    layerStates = stateChanges.find(sourceLayer);
    if (layerStates != stateChanges.end()) {
        const auto stateChangesEntry = layerStates->second.find(featureID);
        if (stateChangesEntry != layerStates->second.end()) {
            changes = stateChangesEntry->second;
        }
    }
    result = std::move(changes);
    result.insert(current.begin(), current.end());
}

void SourceFeatureState::coalesceChanges(std::vector<RenderTile>& tiles) {
    MLN_TRACE_FUNC();

    // Fold the pending sets and removals into the current states. Only a real change moves the
    // revision: setting a state to the value it has, or removing one that is not there, does not.
    bool changed = false;
    for (const auto& [sourceLayer, layerChanges] : stateChanges) {
        auto& layerStates = currentStates[sourceLayer];
        for (const auto& [featureID, featureChanges] : layerChanges) {
            auto& featureState = layerStates[featureID];
            for (const auto& [stateKey, stateVal] : featureChanges) {
                auto current = featureState.find(stateKey);
                if (current == featureState.end()) {
                    featureState.emplace(stateKey, stateVal);
                    changed = true;
                } else if (!(current->second == stateVal)) {
                    current->second = stateVal;
                    changed = true;
                }
            }
        }
    }

    for (const auto& [sourceLayer, layerDeletions] : deletedStates) {
        const auto layerStates = currentStates.find(sourceLayer);
        if (layerStates == currentStates.end()) {
            continue;
        }
        // A removed state stays as an empty entry, so a tile that applies the states later still
        // resets the feature (GeometryTile::setFeatureState).
        if (layerDeletions.empty()) {
            for (auto& [featureID, featureState] : layerStates->second) {
                changed = changed || !featureState.empty();
                featureState.clear();
            }
            continue;
        }
        for (const auto& [featureID, keys] : layerDeletions) {
            const auto featureState = layerStates->second.find(featureID);
            if (featureState == layerStates->second.end()) {
                continue;
            }
            if (keys.empty()) {
                changed = changed || !featureState->second.empty();
                featureState->second.clear();
            } else {
                for (const auto& key : keys) {
                    changed = featureState->second.erase(key.first) > 0 || changed;
                }
            }
        }
    }

    stateChanges.clear();
    deletedStates.clear();
    if (changed) {
        ++revision;
    }

    if (currentStates.empty()) {
        return;
    }

    // Every current state goes to every tile, for tiles loaded or laid out since; a tile that has
    // applied this revision already returns at once.
    for (auto& tile : tiles) {
        tile.setFeatureState(currentStates, revision);
    }
}

void SourceFeatureState::removeState(const std::optional<std::string>& sourceLayerID,
                                     const std::optional<std::string>& featureID,
                                     const std::optional<std::string>& stateKey) {
    std::string sourceLayer = sourceLayerID.value_or(std::string());

    bool sourceLayerDeleted = deletedStates.contains(sourceLayer) && deletedStates[sourceLayer].empty();
    if (sourceLayerDeleted) {
        return;
    }

    if (stateKey && featureID) {
        if (!deletedStates.contains(sourceLayer) && !deletedStates[sourceLayer].contains(*featureID)) {
            deletedStates[sourceLayer][*featureID][*stateKey] = {};
        }
    } else if (featureID) {
        bool updateInQueue = stateChanges.contains(sourceLayer) && stateChanges[sourceLayer].contains(*featureID);
        if (updateInQueue) {
            for (const auto& changeEntry : stateChanges[sourceLayer][*featureID]) {
                deletedStates[sourceLayer][*featureID][changeEntry.first] = {};
            }
        } else {
            deletedStates[sourceLayer][*featureID] = {};
        }
    } else {
        deletedStates[sourceLayer] = {};
    }
}

} // namespace mln
