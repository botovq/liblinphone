/*
 * Copyright (c) 2010-2021 Belledonne Communications SARL.
 *
 * This file is part of Liblinphone.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _L_CONFERENCE_SCHEDULER_H_
#define _L_CONFERENCE_SCHEDULER_H_

#include "c-wrapper/c-wrapper.h"
#include <conference/session/call-session-listener.h>
#include <conference/conference-info.h>
#include "core/core-accessor.h"
#include <chat/chat-room/chat-room-params.h>
#include <conference/session/call-session.h>
#include <belle-sip/object++.hh>
#include "linphone/api/c-types.h"
#include "linphone/types.h"

// =============================================================================

LINPHONE_BEGIN_NAMESPACE

class ConferenceSchedulerCbs;

class LINPHONE_PUBLIC ConferenceScheduler : public bellesip::HybridObject<LinphoneConferenceScheduler, ConferenceScheduler>, 
	public CoreAccessor, 
	public UserDataAccessor, 
	public CallbacksHolder<ConferenceSchedulerCbs>,
	public CallSessionListener
{
public:

	enum class State {
		Idle = LinphoneConferenceSchedulerStateIdle,
		Error = LinphoneConferenceSchedulerStateError,
		AllocationPending = LinphoneConferenceSchedulerStateAllocationPending,
		Ready = LinphoneConferenceSchedulerStateReady,
		Updating = LinphoneConferenceSchedulerStateUpdating
	};

	ConferenceScheduler (const std::shared_ptr<Core> &core);
	virtual ~ConferenceScheduler ();

	void onCallSessionSetTerminated (const std::shared_ptr<CallSession> &session) override;

	State getState () const;

	const std::shared_ptr<ConferenceInfo> getInfo () const;
	void setInfo (std::shared_ptr<ConferenceInfo> info);

	void setConferenceAddress(const ConferenceAddress& conferenceAddress);

	void sendInvitations (std::shared_ptr<ChatRoomParams> chatRoomParams);

private:
	void setState (State newState);
	std::string stateToString (State state);
	
	ConferenceScheduler::State mState;
	std::shared_ptr<ConferenceInfo> mConferenceInfo = nullptr;
	std::shared_ptr<CallSession> mSession = nullptr;
};

class ConferenceSchedulerCbs : public bellesip::HybridObject<LinphoneConferenceSchedulerCbs, ConferenceSchedulerCbs>, public Callbacks {
public:
	LinphoneConferenceSchedulerCbsStateChangedCb getStateChanged () const;
	void setStateChanged (LinphoneConferenceSchedulerCbsStateChangedCb cb);
	LinphoneConferenceSchedulerCbsInvitationsSentCb getInvitationsSent () const;
	void setInvitationsSent (LinphoneConferenceSchedulerCbsInvitationsSentCb cb);

private:
	LinphoneConferenceSchedulerCbsStateChangedCb mStateChangedCb = nullptr;
	LinphoneConferenceSchedulerCbsInvitationsSentCb mInvitationsSent = nullptr;
};

LINPHONE_END_NAMESPACE

#endif // ifndef _L_CONFERENCE_SCHEDULER_H_