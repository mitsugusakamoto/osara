/*
 * OSARA: Open Source Accessibility for the REAPER Application
 * Envelope commands code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2015-2017 NV Access Limited, James Teh
 * License: GNU General Public License version 2.0
 */

#include <windows.h>
#include <string>
#include <sstream>
#include <tuple>
#include <regex>
#include <functional>
#include "osara.h"

using namespace std;

bool selectedEnvelopeIsTake = false;
// Keeps track of the automation item the user last moved to.
// -1 means no automation item,
// which means use the envelope itself for stuff related to envelope points.
int currentAutomationItem = -1;

// Returns the selected envelope and the offset for all envelope point times.
// Track envelope points are relative to the start of the project,
// but take envelope points are relative to the start of the item.
pair<TrackEnvelope*, double> getSelectedEnvelopeAndOffset() {
	TrackEnvelope* envelope = GetSelectedEnvelope(0);
	if (!envelope)
		return {NULL, 0.0};
	if (!selectedEnvelopeIsTake)
		return {envelope, 0.0};
	MediaItem* item = GetSelectedMediaItem(0, 0);
	if (!item)
		return {envelope, 0.0};
	double offset = *(double*)GetSetMediaItemInfo(item, "D_POSITION", NULL);
	return {envelope, offset};
}

void postMoveEnvelopePoint(int command) {
	TrackEnvelope* envelope;
	double offset;
	tie(envelope, offset) = getSelectedEnvelopeAndOffset();
	if (!envelope)
		return;
	fakeFocus = FOCUS_ENVELOPE;
	// GetEnvelopePointByTime often returns the point before instead of right at the position.
	// Increment the cursor position a bit to work around this.
	int point = GetEnvelopePointByTime(envelope, GetCursorPosition() + 0.0001 - offset);
	if (point < 0)
		return;
	double value;
	bool selected;
	GetEnvelopePoint(envelope, point, NULL, &value, NULL, NULL, &selected);
	if (!selected)
		return; // Not moved.
	char out[64];
	Envelope_FormatValue(envelope, value, out, sizeof(out));
	outputMessage(out);
}

void cmdhDeleteEnvelopePointsOrAutoItems(int command, bool checkPoints, bool checkItems) {
	TrackEnvelope* envelope = GetSelectedEnvelope(0);
	if (!envelope)
		return;
	int oldPoints;
	int oldItems;
	if (checkPoints) {
		oldPoints = CountEnvelopePoints(envelope);
	}
	if (checkItems) {
		oldItems = CountAutomationItems(envelope);
	}
	Main_OnCommand(command, 0);
	ostringstream s;
	int removed;
	if (checkItems) {
		removed = oldItems - CountAutomationItems(envelope);
		s << removed << (removed == 1 ? " automation item" : " automation items") << " removed";
		outputMessage(s);
		return;
	}
	if (checkPoints) {
		removed = oldPoints - CountEnvelopePoints(envelope);
		s << removed << (removed == 1 ? " point" : " points") << " removed";
		outputMessage(s);
	}
}

void cmdDeleteEnvelopePoints(Command* command) {
	cmdhDeleteEnvelopePointsOrAutoItems(command->gaccel.accel.cmd, true, false);
}

void moveToEnvelopePoint(int direction, bool clearSelection=true) {
	TrackEnvelope* envelope;
	double offset;
	tie(envelope, offset) = getSelectedEnvelopeAndOffset();
	if (!envelope)
		return;
	int count = CountEnvelopePoints(envelope);
	if (count == 0)
		return;
	double now = GetCursorPosition();
	// Get the point at or before the cursr.
	int point = GetEnvelopePointByTime(envelope, now - offset);
	if (point < 0) {
		if (direction != 1)
			return;
		++point;
	}
	double time, value;
	bool selected;
	GetEnvelopePoint(envelope, point, &time, &value, NULL, NULL, &selected);
	time += offset;
	if ((direction == 1 && time < now)
		// If this point is at the cursor, skip it only if it's selected.
		// This allows you to easily get to a point at the cursor
		// while still allowing you to move beyond it once you do.
		|| (direction == 1 && selected && time == now)
		// Moving backward should skip the point at the cursor.
		|| (direction == -1 && time >= now)
	) {
		// This isn't the point we want. Try the next.
		int newPoint = point + direction;
		if (0 <= newPoint && newPoint < count) {
			point = newPoint;
			GetEnvelopePoint(envelope, point, &time, &value, NULL, NULL, &selected);
			time += offset;
		}
	}
	if (direction != 0 && direction == 1 ? time < now : time > now)
		return; // No point in this direction.
	fakeFocus = FOCUS_ENVELOPE;
	if (clearSelection)
		Main_OnCommand(40331, 0); // Envelope: Unselect all points
	SetEnvelopePoint(envelope, point, NULL, NULL, NULL, NULL, &bTrue, &bTrue);
	if (direction != 0)
		SetEditCurPos(time, true, true);
	ostringstream s;
	s << "point " << point + 1 << " value ";
	char out[64];
	Envelope_FormatValue(envelope, value, out, sizeof(out));
	s << out;
	if (!clearSelection) {
		int numSel = 0;
		for (point = 0; point < count; ++point) {
			GetEnvelopePoint(envelope, point, NULL, NULL, NULL, NULL, &selected);
			if (selected)
				++numSel;
			if (numSel == 2)
				break; // Don't care above this.
		}
		// One selected point is the norm, so don't report selected in this case.
		if (numSel > 1)
			s << " selected";
	}
	s << " " << formatCursorPosition();
	outputMessage(s);
}

void cmdInsertEnvelopePoint(Command* command) {
	TrackEnvelope* envelope = GetSelectedEnvelope(0);
	if (!envelope)
		return;
	int oldCount = CountEnvelopePoints(envelope);
	Main_OnCommand(command->gaccel.accel.cmd, 0);
	if (CountEnvelopePoints(envelope) <= oldCount)
		return;
	moveToEnvelopePoint(0); // Select and report inserted point.
}

const regex RE_ENVELOPE_STATE("<(AUX|HW)?(\\S+)[^]*?\\sACT (0|1)[^]*?\\sVIS (0|1)[^]*?\\sARM (0|1)");
void cmdhSelectEnvelope(int direction) {
	// If we're focused on a track or item, use the envelopes associated therewith.
	// If we're focused on an envelope, use what we last used.
	// This is necessary because the first envelope selection will focus the envelope
	// and we want to allow the user to move past the first.
	if (fakeFocus == FOCUS_TRACK)
		selectedEnvelopeIsTake = false;
	else if (fakeFocus == FOCUS_ITEM)
		selectedEnvelopeIsTake = true;
	else if (fakeFocus != FOCUS_ENVELOPE && fakeFocus != FOCUS_AUTOMATIONITEM)
		return; // No envelopes for focus.
	MediaTrack* track = NULL;
	int count;
	function<TrackEnvelope*(int)> getEnvelope;
	if (selectedEnvelopeIsTake) {
		MediaItem* item = GetSelectedMediaItem(0, 0);
		if (!item)
			return;
		MediaItem_Take* take = GetActiveTake(item);
		if (!take)
			return;
		count = CountTakeEnvelopes(take);
		getEnvelope = [take] (int index) { return GetTakeEnvelope(take, index); };
	} else {
		track = GetLastTouchedTrack();
		if (!track)
			return;
		count = CountTrackEnvelopes(track);
		getEnvelope = [track] (int index) { return GetTrackEnvelope(track, index); };
	}
	if (count == 0) {
		outputMessage(selectedEnvelopeIsTake ? "no take envelopes" : "no track envelopes");
		return;
	}

	TrackEnvelope* origEnv = GetSelectedEnvelope(0);
	int start = direction == 1 ? 0 : count - 1;
	// Find the current envelope.
	int origIndex = -1;
	int index;
	TrackEnvelope* env;
	for (index = start; 0 <= index && index < count; index += direction) {
		env = getEnvelope(index);
		if (env == origEnv) {
			origIndex = index;
			break;
		}
	}
	if (origIndex == -1) {
		// The current envelope isn't for this track/take.
		origEnv = NULL;
		// Start at the start.
		origIndex = start - direction;
	}

	// Get the next envelope in the requested direction.
	cmatch m;
	index = origIndex;
	for (; ;) {
		index += direction;
		if (index < 0 || index >= count) {
			if (origEnv) {
				// We started after the start, so wrap around.
				index = start;
			} else {
				// We started at the start, so there are no more.
				env = NULL;
				break;
			}
		}
		env = getEnvelope(index);
		char state[100];
		GetEnvelopeStateChunk(env, state, sizeof(state), false);
		regex_search(state, m, RE_ENVELOPE_STATE);
		if (env == origEnv) {
			// We're back where we started. Don't try to go any further.
			break;
		}
		if (!m.empty() && m.str(4)[0] == '0')
			continue; // Invisible, so skip.
		break; // We found our envelope!
	}
	if (!env) {
		outputMessage("no visible envelopes");
		return;
	}

	SetCursorContext(2, env);
	currentAutomationItem = -1;
	fakeFocus = FOCUS_ENVELOPE;
	ostringstream s;
	if (!m.empty() && m.str(1).compare("AUX") == 0) {
		// Send envelope. Get the name of the send.
		string envType = '<' + m.str(2); // e.g. <VOLENV
		int sendCount = GetTrackNumSends(track, 0);
		for (int i = 0; i < sendCount; ++i) {
			TrackEnvelope* sendEnv = (TrackEnvelope*)GetSetTrackSendInfo(track, 0, i, "P_ENV", (void*)envType.c_str());
			if (sendEnv == env) {
				MediaTrack* sendTrack = (MediaTrack*)GetSetTrackSendInfo(track, 0, i, "P_DESTTRACK", NULL);
				s << (int)(size_t)GetSetMediaTrackInfo(sendTrack, "IP_TRACKNUMBER", NULL) << " ";
				char* trackName = (char*)GetSetMediaTrackInfo(sendTrack, "P_NAME", NULL);
				if (trackName)
					s << trackName << " ";
			}
		}
	}
	char name[50];
	GetEnvelopeName(env, name, sizeof(name));
	s << name << " envelope";
	if (!m.empty()) {
		if (m.str(3)[0] == '0')
			s << " bypassed";
		if (m.str(5)[0] == '1')
			s << " armed";
	}
	outputMessage(s);
}

void cmdSelectNextEnvelope(Command* command) {
	cmdhSelectEnvelope(1);
}

void cmdSelectPreviousEnvelope(Command* command) {
	cmdhSelectEnvelope(-1);
}

void cmdMoveToNextEnvelopePoint(Command* command) {
	moveToEnvelopePoint(1);
}

void cmdMoveToPrevEnvelopePoint(Command* command) {
	moveToEnvelopePoint(-1);
}

void cmdMoveToNextEnvelopePointKeepSel(Command* command) {
	moveToEnvelopePoint(1, false);
}

void cmdMoveToPrevEnvelopePointKeepSel(Command* command) {
	moveToEnvelopePoint(-1, false);
}

void selectAutomationItem(TrackEnvelope* envelope, int index, bool select=true) {
	GetSetAutomationItemInfo(envelope, index, "D_UISEL", select, true);
}

void unselectAllAutomationItems(TrackEnvelope* envelope) {
	int count = CountAutomationItems(envelope);
	for (int i = 0; i < count; ++i) {
		selectAutomationItem(envelope, i, false);
	}
}

bool isAutomationItemSelected(TrackEnvelope* envelope, int index) {
	return GetSetAutomationItemInfo(envelope, index, "D_UISEL", 0, false);
}

// Return whether there are 0, 1 or >= 2 automation items selected.
// That is, if there are 3 or more selected, only 2 will be returned.
int countSelectedAutomationItems(TrackEnvelope* envelope) {
	int count = CountAutomationItems(envelope);
	int sel = 0;
	for (int i = 0; i < count; ++i) {
		if (isAutomationItemSelected(envelope, i)) {
			++sel;
		}
		if (sel == 2) {
			// optimisation: We don't care beyond 2.
			return sel;
		}
	}
	return sel;
}

void moveToAutomationItem(int direction, bool clearSelection=true, bool select=true) {
	TrackEnvelope* envelope = GetSelectedEnvelope(0);
	if (!envelope) {
		return;
	}
	int count = CountAutomationItems(envelope);
	if (!count) {
		return;
	}
	double cursor = GetCursorPosition();
	double pos;
	int start = direction == 1 ? 0 : count - 1;
	if (0 <= currentAutomationItem && currentAutomationItem < count) {
		pos = GetSetAutomationItemInfo(envelope, currentAutomationItem, "D_POSITION", 0, false);
		if (direction == 1 ? pos <= cursor : pos >= cursor) {
			// The cursor is right at or has moved past the automation item to which the user last moved.
			// Therefore, start at the adjacent automation item.
			// This is faster and also allows the user to move to automation items which start at the same position.
			start += direction;
			if (start < 0 || start >= count) {
				// There's no adjacent automation item in this direction,
				// so move to the current one again.
				start -= direction;
			}
		}
	} else {
		currentAutomationItem = -1; // Invalid.
	}

	for (int i = start; 0 <= i && i < count; i += direction) {
		pos = GetSetAutomationItemInfo(envelope, i, "D_POSITION", 0, false);
		if (direction == 1 ? pos < cursor : pos > cursor) {
			continue; // Not the right direction.
		}
		currentAutomationItem = i;
		if (clearSelection || select) {
			Undo_BeginBlock();
		}
		if (clearSelection) {
			unselectAllAutomationItems(envelope);
			isSelectionContiguous = true;
		}
		if (select) {
			selectAutomationItem(envelope, i);
		}
		if (clearSelection || select) {
			Undo_EndBlock("Change Automation Item Selection", 0);
		}
		SetEditCurPos(pos, true, true); // Seek playback.

		// Report the automation item.
		fakeFocus = FOCUS_AUTOMATIONITEM;
		ostringstream s;
		s << "Auto " << i + 1;
		if (isAutomationItemSelected(envelope, i)) {
			// One selected item is the norm, so don't report selected in this case.
			if (countSelectedAutomationItems(envelope) > 1) {
				s << " selected";
			}
		} else {
			s << " unselected";
		}
		s << " " << formatCursorPosition();
		outputMessage(s);
		return;
	}
}

bool toggleCurrentAutomationItemSelection() {
	TrackEnvelope* envelope = GetSelectedEnvelope(0);
	if (!envelope || currentAutomationItem == -1) {
		// We really shouldn't get called if this happens, but just in case...
		return false;
	}
	bool select = !isAutomationItemSelected(envelope, currentAutomationItem);
	selectAutomationItem(envelope, currentAutomationItem, select);
	return select;
}
