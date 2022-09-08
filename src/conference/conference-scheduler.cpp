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
 * but WITHOUT ANY WARRANTY{
}
 without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "conference-scheduler.h"
#include "participant.h"
#include "chat/chat-message/chat-message-p.h"
#include "conference/params/call-session-params-p.h"
#include "conference/session/media-session.h"
#include "core/core-p.h"
#include "content/file-content.h"
#include "account/account.h"

// =============================================================================

using namespace std;

LINPHONE_BEGIN_NAMESPACE

ConferenceScheduler::ConferenceScheduler (
	const shared_ptr<Core> &core
	) : CoreAccessor(core) {
	mState = State::Idle;
	auto default_account = linphone_core_get_default_account(core->getCCore());
	if (default_account) {
		mAccount =  Account::toCpp(default_account)->getSharedFromThis();
	}
}

ConferenceScheduler::~ConferenceScheduler () {
	if (mSession != nullptr) {
		mSession->setListener(nullptr);
	}
	if (mAccount) {
		mAccount = nullptr;
	}
}

const std::shared_ptr<Account> & ConferenceScheduler::getAccount() const {
	return mAccount;
}

void ConferenceScheduler::setAccount(std::shared_ptr<Account> account) {
	if ((mState == State::Idle) || (mState == State::AllocationPending) || (mState == State::Error)) {
		mAccount = account;
	} else {
		lWarning() << "[Conference Scheduler] [" << this << "] Unable to change account because scheduler is in state " << mState;
	}
}

ConferenceScheduler::State ConferenceScheduler::getState () const {
	return mState;
}

void ConferenceScheduler::setState (State newState) {
	if (mState != newState) {
		lInfo() << "[Conference Scheduler] [" << this << "] moving from state " << mState << " to state " << newState;
		mState = newState;
		linphone_conference_scheduler_notify_state_changed(toC(), (LinphoneConferenceSchedulerState)newState);
	}
}

const std::shared_ptr<ConferenceInfo> ConferenceScheduler::getInfo () const {
	return mConferenceInfo;
}

void ConferenceScheduler::fillCancelList(const std::list<IdentityAddress> &oldList, const std::list<IdentityAddress> &newList) {
	mCancelToSend.clear();
	for (const auto & address : oldList) {
		const bool participantFound = (std::find(newList.cbegin(), newList.cend(), address) != newList.cend());
		if (!participantFound) {
			mCancelToSend.push_back(address);
		}
	}
}

void ConferenceScheduler::setInfo (std::shared_ptr<ConferenceInfo> info) {
	if (!info) {
		lWarning() << "[Conference Scheduler] [" << this << "] Trying to set null conference info to the conference scheduler. Aborting conference creation!";
		setState(State::Error);
		return;
	}

	if (!mAccount) {
		auto default_account = linphone_core_get_default_account(getCore()->getCCore());
		if (default_account) {
			mAccount =  Account::toCpp(default_account)->getSharedFromThis();
		}
	}

	const auto creator =  mAccount ? IdentityAddress(*L_GET_CPP_PTR_FROM_C_OBJECT(mAccount->getAccountParams()->getIdentityAddress())) : IdentityAddress(linphone_core_get_identity(getCore()->getCCore()));
	if (!creator.isValid()) {
		lWarning() << "[Conference Scheduler] [" << this << "] Core address attempting to set conference information!";
		return;
	}

	const auto & organizer = info->getOrganizer();
	const auto & participants = info->getParticipants();
	const auto participantListEmpty = participants.empty();
	const bool participantFound = (std::find(participants.cbegin(), participants.cend(), creator) != participants.cend());
	if ((creator != organizer) && !participantFound) {
		lWarning() << "[Conference Scheduler] [" << this << "] Unable to find the address " << creator << " setting the conference information among the list of participants or the organizer (" << info->getOrganizer() << ") of conference " << info->getUri();
		setState(State::Error);
		return;
	}

	bool isUpdate = false;
	ConferenceAddress conferenceAddress;
#ifdef HAVE_DB_STORAGE
	if (info->getUri().isValid()) {
		auto &mainDb = getCore()->getPrivate()->mainDb;
		auto confInfo = mainDb->getConferenceInfoFromURI(info->getUri());
		if (confInfo) {
			lInfo() << "[Conference Scheduler] [" << this << "] Found matching conference info in database for address [" << info->getUri() << "]";
			conferenceAddress = info->getUri();
			isUpdate = true;
			setState(State::Updating);
			info->setIcsUid(confInfo->getIcsUid());
			info->setIcsSequence(confInfo->getIcsSequence() + 1);
			fillCancelList(confInfo->getParticipants(), info->getParticipants());
		}
	}
#endif // HAVE_DB_STORAGE

	if (participantListEmpty && !isUpdate) {
		lWarning() << "[Conference Scheduler] [" << this << "] Can't create a scheduled conference if no participants are added!";
		setState(State::Error);
		return;
	}

	if (mConferenceInfo == nullptr && !isUpdate) {
		setState(State::AllocationPending);
		if (info->getUri().isValid()) {
			// This is a hack for the tester
			lError() << "[Conference Scheduler] [" << this << "] This is a hack for liblinphone-tester, you shouldn't see this in production!";
			mConferenceInfo = info;
			setState(State::Ready);
			return;
		}
	} else if (mConferenceInfo != nullptr) {
		conferenceAddress = mConferenceInfo->getUri();
		info->setUri(conferenceAddress);
		setState(State::Updating);
		fillCancelList(mConferenceInfo->getParticipants(), info->getParticipants());
	}

	auto infoState = ConferenceInfo::State::New;
	if (getState() == State::Updating) {
		if (info->getParticipants().size() == 0) {
			infoState = ConferenceInfo::State::Cancelled;
		} else {
			infoState = ConferenceInfo::State::Updated;
		}
	}
	info->setState(infoState);
	mConferenceInfo = info;

	shared_ptr<LinphonePrivate::ConferenceParams> conferenceParams = ConferenceParams::create(getCore()->getCCore());
	const auto identityAddress = mConferenceInfo->getOrganizer();
	conferenceParams->enableAudio(true);
	conferenceParams->enableVideo(true);
	conferenceParams->setSubject(mConferenceInfo->getSubject());

	if (mConferenceInfo->getDateTime() <= 0) {
		if (!isUpdate) {
			// Set start time only if a conference is going to be created
			mConferenceInfo->setDateTime(ms_time(NULL));
		}
	} else {
		const auto & startTime = info->getDateTime();
		conferenceParams->setStartTime(startTime);
		const auto & duration = info->getDuration();
		if (duration > 0) {
			const auto endTime = startTime + static_cast<time_t>(duration) * 60; // duration is in minutes
			conferenceParams->setEndTime(endTime);
		}
	}

	if (isUpdate) {
		// Updating an existing conference
		mSession = getCore()->createOrUpdateConferenceOnServer(conferenceParams, creator, mConferenceInfo->getParticipants(), conferenceAddress);
	} else {
		// Creating conference
		mSession = getCore()->createConferenceOnServer(conferenceParams, identityAddress, mConferenceInfo->getParticipants());
	}
	if (mSession == nullptr) {
		lError() << "[Conference Scheduler] [" << this << "] createConferenceOnServer returned a null session!";
		setState(State::Error);
		return;
	}
	mSession->setListener(this);

	// Update conference info in database with updated conference information
#ifdef HAVE_DB_STORAGE
	auto &mainDb = getCore()->getPrivate()->mainDb;
	mainDb->insertConferenceInfo(mConferenceInfo);
#endif // HAVE_DB_STORAGE
}

void ConferenceScheduler::onChatMessageStateChanged (const shared_ptr<ChatMessage> &message, ChatMessage::State state) {
	shared_ptr<AbstractChatRoom> chatRoom = message->getChatRoom();
	IdentityAddress participantAddress = message->getRecipientAddress();

	if (state == ChatMessage::State::NotDelivered) { // Message wasn't delivered
		if (chatRoom->getState() == Conference::State::Created) { // Chat room was created successfully
			if (chatRoom->getCapabilities() & ChatRoom::Capabilities::OneToOne) { // Message was sent using a 1-1 chat room
				lError() << "[Conference Scheduler] [" << this << "] Invitation couldn't be sent to participant [" << participantAddress << "]";
				mInvitationsInError.push_back(Address(participantAddress.asAddress()));
			} else { // Message was sent using a group chat room
				lError() << "[Conference Scheduler] [" << this << "] At least some participants of the chat room haven't received the invitation!";
				mInvitationsSent += (unsigned long)(message->getParticipantsByImdnState(ChatMessage::State::Delivered).size());
				for (auto &participant : message->getParticipantsByImdnState(ChatMessage::State::NotDelivered)) {
					participantAddress = participant.getParticipant()->getAddress();
					lError() << "[Conference Scheduler] [" << this << "] Invitation couldn't be sent to participant [" << participantAddress << "]";
					mInvitationsInError.push_back(Address(participantAddress.asAddress()));
				}
			}
		} else { // Chat room wasn't created
			if (chatRoom->getCapabilities() & ChatRoom::Capabilities::OneToOne) { // Message was sent using a 1-1 chat room
				lError() << "[Conference Scheduler] [" << this << "] Invitation couldn't be sent to participant [" << participantAddress << "]";
				mInvitationsInError.push_back(Address(participantAddress.asAddress()));
			} else { // Message was sent using a group chat room
				lError() << "[Conference Scheduler] [" << this << "] Chat room wasn't creatd, so no one received the invitation!";
				for (auto &participant : mInvitationsToSend) {
					mInvitationsInError.push_back(Address(participant.asAddress()));
				}
			}
		}
	} else if (state == ChatMessage::State::Delivered || 
			state == ChatMessage::State::DeliveredToUser ||
			state == ChatMessage::State::Displayed) { // Message was delivered (first received state can be any of those 3)
		lInfo() << "[Conference Scheduler] [" << this << "] Invitation to participant [" << participantAddress << "] was delivered (" << state << ")";
		mInvitationsSent += 1;
		message->removeListener(getSharedFromThis());
	} else {
		return;
	}

	if (mInvitationsSent + mInvitationsInError.size() == mInvitationsToSend.size()) {
		linphone_conference_scheduler_notify_invitations_sent(toC(), L_GET_RESOLVED_C_LIST_FROM_CPP_LIST(mInvitationsInError));
	}
}

void ConferenceScheduler::setConferenceAddress(const ConferenceAddress& conferenceAddress) {
	if (mConferenceInfo == nullptr) {
		lError() << "[Conference Scheduler] [" << this << "] Can't update conference address " << conferenceAddress << " on null conference info";
		setState(State::Error);
		return;
	}

	mConferenceInfo->setUri(conferenceAddress);

#ifdef HAVE_DB_STORAGE
	auto &mainDb = getCore()->getPrivate()->mainDb;
	if (mainDb) {
		lInfo() << "[Conference Scheduler] [" << this << "] Conference address " << conferenceAddress << " is known, inserting conference info in database";
		mainDb->insertConferenceInfo(mConferenceInfo);
	}
#endif

	setState(State::Ready);
}

void ConferenceScheduler::onCallSessionSetTerminated (const shared_ptr<CallSession> &session) {
	const Address *remoteAddress = session->getRemoteContactAddress();
	if (remoteAddress == nullptr) {
		auto conferenceAddress = mConferenceInfo->getUri();
		lError() << "[Conference Scheduler] [" << this << "] The session to update the conference information of conference " << (conferenceAddress.isValid() ? conferenceAddress.asString() : std::string("<unknown-address>")) << " did not succesfully establish hence it is likely that the request wasn't taken into account by the server";
		setState(State::Error);
	} else {
		// Do not try to call inpromptu conference if a participant updates its informations
		if ((getState() == State::AllocationPending) && (session->getParams()->getPrivate()->getStartTime() < 0)) {
			lInfo() << "Automatically rejoining conference " << *remoteAddress;
			auto new_params = linphone_core_create_call_params(getCore()->getCCore(), nullptr);
			// Participant with the focus call is admin
			L_GET_CPP_PTR_FROM_C_OBJECT(new_params)->addCustomContactParameter("admin", Utils::toString(true));
			auto addressesList(mConferenceInfo->getParticipants());

			addressesList.sort();
			addressesList.unique();

			if (!addressesList.empty()) {
				Content content;
				content.setBodyFromUtf8(Utils::getResourceLists(addressesList));
				content.setContentType(ContentType::ResourceLists);
				content.setContentDisposition(ContentDisposition::RecipientList);
				if (linphone_core_content_encoding_supported(getCore()->getCCore(), "deflate")) {
					content.setContentEncoding("deflate");
				}

				L_GET_CPP_PTR_FROM_C_OBJECT(new_params)->addCustomContent(content);
			}
			linphone_call_params_enable_video(new_params, static_pointer_cast<MediaSession>(session)->getMediaParams()->videoEnabled());

			linphone_core_invite_address_with_params_2(getCore()->getCCore(), L_GET_C_BACK_PTR(remoteAddress), new_params, L_STRING_TO_C(mConferenceInfo->getSubject()), NULL);
			linphone_call_params_unref(new_params);
		}

		auto conferenceAddress = ConferenceAddress(*remoteAddress);
		lInfo () << "[Conference Scheduler] [" << this << "] Conference has been succesfully created: " << conferenceAddress;
		setConferenceAddress(conferenceAddress);
	}
}

void ConferenceScheduler::onCallSessionStateChanged (const shared_ptr<CallSession> &session, CallSession::State state, const string &message) {
	switch(state) {
		case CallSession::State::StreamsRunning:
			session->terminate();
			break;
		default:
			break;
	}
}

shared_ptr<ChatMessage> ConferenceScheduler::createInvitationChatMessage(shared_ptr<AbstractChatRoom> chatRoom, bool cancel) {
	shared_ptr<LinphonePrivate::ChatMessage> message;
	if (linphone_core_conference_ics_in_message_body_enabled(chatRoom->getCore()->getCCore())) {
		message = chatRoom->createChatMessageFromUtf8(mConferenceInfo->toIcsString(cancel));
		message->getPrivate()->setContentType(ContentType::Icalendar);
	} else {
		FileContent *content = new FileContent(); // content will be deleted by ChatMessage
		content->setContentType(ContentType::Icalendar);
		content->setFileName("conference.ics");
		content->setBodyFromUtf8(mConferenceInfo->toIcsString(cancel));
		message = chatRoom->createFileTransferMessage(content);
	}

	// Update conference info in database with new sequence and uid
#ifdef HAVE_DB_STORAGE
	auto &mainDb = getCore()->getPrivate()->mainDb;
	mainDb->insertConferenceInfo(mConferenceInfo);
#endif // HAVE_DB_STORAGE
	message->addListener(getSharedFromThis());
	return message;
}

void ConferenceScheduler::sendInvitations (shared_ptr<ChatRoomParams> chatRoomParams) {
	if (mState != State::Ready) {
		lWarning() << "[Conference Scheduler] [" << this << "] Can't send conference invitation if state ins't Ready, current state is " << mState;
		return;
	}

	if (!mAccount) {
		auto default_account = linphone_core_get_default_account(getCore()->getCCore());
		if (default_account) {
			mAccount =  Account::toCpp(default_account)->getSharedFromThis();
		}
	}

	const auto sender =  mAccount ? IdentityAddress(*L_GET_CPP_PTR_FROM_C_OBJECT(mAccount->getAccountParams()->getIdentityAddress())) : IdentityAddress(linphone_core_get_identity(getCore()->getCCore()));
	if (!sender.isValid()) {
		lWarning() << "[Conference Scheduler] [" << this << "] Core address attempting to send invitation isn't valid!";
		return;
	}

	const auto & participants = mConferenceInfo->getParticipants();
	const bool participantFound = (std::find(participants.cbegin(), participants.cend(), sender) != participants.cend());
	if ((sender != mConferenceInfo->getOrganizer()) && !participantFound) {
		lWarning() << "[Conference Scheduler] [" << this << "] Unable to find the address " << sender << " sending invitations among the list of participants or the organizer (" << mConferenceInfo->getOrganizer() << ") of conference " << mConferenceInfo->getUri();
		return;
	}

	if (chatRoomParams->isGroup()) {
		lError() << "[Conference Scheduler] [" << this << "] Unable to send invitations to a group chat. Participant must be notified using individual chat rooms.";
		return;
	}
	if (!chatRoomParams->isValid()) {
		lWarning() << "[Conference Scheduler] [" << this << "] Given chat room params aren't valid!";
		return;
	}

	auto invitees = participants;
	invitees.insert(invitees.begin(), mCancelToSend.begin(), mCancelToSend.end());

	mInvitationsToSend.clear();
	for (auto participant : invitees) {
		if (participant != sender) {
			mInvitationsToSend.push_back(Address(participant.asAddress()));
		} else {
			lInfo() << "[Conference Scheduler] [" << this << "] Removed conference participant [" << participant << "] from chat room participants as it is ourselves";
		}
	}

	const auto & organizer = mConferenceInfo->getOrganizer();
	if (sender != organizer) {
		const bool organizerFound = (std::find(mInvitationsToSend.cbegin(), mInvitationsToSend.cend(), organizer) != mInvitationsToSend.cend());
		if (!organizerFound) {
			lInfo() << "[Conference Scheduler] [" << this << "] Organizer [" << organizer << "] not found in conference participants, adding it to chat room participants";
			mInvitationsToSend.push_back(Address(organizer.asAddress()));
		}
		const bool organizerInInviteesFound = (std::find(invitees.cbegin(), invitees.cend(), organizer) != invitees.cend());
		if (!organizerInInviteesFound) {
			invitees.push_back(Address(organizer.asAddress()));
		}
	}

	mInvitationsInError.clear();
	mInvitationsSent = 0;

	// Sending the ICS once for each participant in a separated chat room each time.
	for (auto participant : invitees) {
		list<IdentityAddress> participantList;
		participantList.push_back(participant);

		shared_ptr<AbstractChatRoom> chatRoom = getCore()->getPrivate()->searchChatRoom(
			chatRoomParams,
			sender,
			IdentityAddress(),
			participantList);

		if (!chatRoom) {
			lInfo() << "[Conference Scheduler] [" << this << "] Existing chat room between [" << sender << "] and [" << participant << "] wasn't found, creating it.";
			chatRoom = getCore()->getPrivate()->createChatRoom(
				chatRoomParams,
				sender,
				participantList);
		} else {
			lInfo() << "[Conference Scheduler] [" << this << "] Found existing chat room [" << chatRoom->getPeerAddress() << "] between [" << sender << "] and [" << participant << "], using it";
		}

		if (!chatRoom) {
			lError() << "[Conference Scheduler] [" << this << "] Couldn't find nor create a chat room between [" << sender << "] and [" << participant << "]";
			mInvitationsInError.push_back(Address(participant.asAddress()));
			continue;
		}

		const bool cancel = (std::find(mCancelToSend.cbegin(), mCancelToSend.cend(), participant) != mCancelToSend.cend()) || (mConferenceInfo->getState() == ConferenceInfo::State::Cancelled);

		shared_ptr<ChatMessage> message = createInvitationChatMessage(chatRoom, cancel);
		message->getPrivate()->setRecipientAddress(participant);
		message->send();
	}
}

string ConferenceScheduler::stateToString (ConferenceScheduler::State state) {
	switch (state) {
		case ConferenceScheduler::State::AllocationPending:
			return "AllocationPending";
		case ConferenceScheduler::State::Error:
			return "Error";
		case ConferenceScheduler::State::Ready:
			return "Ready";
		case ConferenceScheduler::State::Updating:
			return "Updating";
		case ConferenceScheduler::State::Idle:
			return "Idle";
	}
	return "<unknown>";
}

std::ostream& operator<<(std::ostream& lhs, ConferenceScheduler::State s) {
	switch (s) {
		case ConferenceScheduler::State::AllocationPending:
			return lhs << "AllocationPending";
		case ConferenceScheduler::State::Error:
			return lhs << "Error";
		case ConferenceScheduler::State::Ready:
			return lhs << "Ready";
		case ConferenceScheduler::State::Updating:
			return lhs << "Updating";
		case ConferenceScheduler::State::Idle:
			return lhs << "Idle";
	}
	return lhs;
}

LinphoneConferenceSchedulerCbsStateChangedCb ConferenceSchedulerCbs::getStateChanged() const {
	return mStateChangedCb;
}

void ConferenceSchedulerCbs::setStateChanged(LinphoneConferenceSchedulerCbsStateChangedCb cb) {
	mStateChangedCb = cb;
}

LinphoneConferenceSchedulerCbsInvitationsSentCb ConferenceSchedulerCbs::getInvitationsSent() const {
	return mInvitationsSent;
}

void ConferenceSchedulerCbs::setInvitationsSent(LinphoneConferenceSchedulerCbsInvitationsSentCb cb) {
	mInvitationsSent = cb;
}

LINPHONE_END_NAMESPACE
