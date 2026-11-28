// SPDX-License-Identifier: Apache-2.0
// Volume arrival and removal (see geaos.h, "Volume monitoring").
#include "geaos.h"

#include <algorithm>

namespace gea::geaos {

void VolumeMonitor::add(VolumeSource *source)
{
	if (!source) return;
	for (const Entry &entry : sources_)
		if (entry.source == source) return;
	sources_.push_back({source, false});
}

void VolumeMonitor::remove(VolumeSource *source)
{
	for (auto it = sources_.begin(); it != sources_.end(); ++it) {
		if (it->source != source) continue;
		if (it->mounted) {
			Launcher::removeVolume(source->volume().id);
			source->lost();
		}
		sources_.erase(it);
		return;
	}
}

bool VolumeMonitor::poll(bool rescanUnchanged)
{
	bool moved = false;
	for (Entry &entry : sources_) {
		const bool present = entry.source->probe();
		if (present == entry.mounted) continue;
		moved = true;
		entry.mounted = present;
		const Volume volume = entry.source->volume();
		if (!present) {
			Launcher::removeVolume(volume.id);
			entry.source->lost();
			continue;
		}
		// What an install cut short when this volume last went away (or
		// the power did) is settled before anything lists it.
		recoverInstalls(Launcher::fs(), volume);
		Launcher::addVolume(volume);
	}
	if (moved) {
		// Volume order is source order, whatever order they arrived in: an
		// earlier volume wins a version tie, so a card re-inserted must not
		// jump ahead of internal storage.
		std::vector<Volume> current = Launcher::volumes();
		std::vector<Volume> ordered;
		for (const Entry &entry : sources_) {
			if (!entry.mounted) continue;
			const std::string id = entry.source->volume().id;
			auto it = std::find_if(current.begin(), current.end(), [&](const Volume &v) { return v.id == id; });
			if (it != current.end()) ordered.push_back(*it);
		}
		// Volumes added outside the monitor (a board's fixed mount) keep
		// their place after the monitored ones.
		for (const Volume &volume : current)
			if (std::none_of(ordered.begin(), ordered.end(), [&](const Volume &v) { return v.id == volume.id; })) ordered.push_back(volume);
		std::vector<std::string> ids;
		for (const Volume &volume : ordered) ids.push_back(volume.id);
		Launcher::orderVolumes(ids);
	}
	// Rescan on every poll, not only on a mount change: a file copied onto a
	// mounted volume (a desktop folder, a card written over USB mass storage)
	// changes nothing the source can see. An unchanged volume rescans to the
	// same signature and reports no change.
	if (!moved && !rescanUnchanged) return false;
	return Launcher::rescan();
}

void VolumeMonitor::clear()
{
	sources_.clear();
}

}  // namespace gea::geaos
