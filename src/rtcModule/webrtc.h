#ifndef IRTC_MODULE_H
#define IRTC_MODULE_H
/**
 * @file IRtcModule.h
 * @brief Public interface of the webrtc module
 *
 * (c) 2013-2015 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */
#ifdef KARERE_DISABLE_WEBRTC
namespace rtcModule
{
class IVideoRenderer;
class IGlobalHandler {};
class ICallHandler {};
class ISessionHandler {};
class IRtcCrypto {};
class ISession {};
class ICall {};
class IRtcModule;
class RtcModule;
}

#else

#include "karereCommon.h" //for AvFlags
#include <karereId.h>
#include <trackDelete.h>
#include <serverListProviderForwards.h>
namespace chatd
{
    class Connection;
    class Client;
}

namespace rtcModule
{
class IVideoRenderer;
struct RtMessage;
class RtcModule;
class IRtcModule;
class Call;
class ICall;
class Session;
class ISession;
class IGlobalHandler;
class ICallHandler;
class ISessionHandler;
class IRtcCrypto;
enum: uint8_t
{
    RTCMD_CALL_REQUEST = 0, // initiate new call, receivers start ringing
    RTCMD_CALL_RINGING = 1, // notifies caller that there is a receiver and it is ringing
    RTCMD_CALL_REQ_DECLINE = 2, // decline incoming call request, with specified Term code
    // (can be only kBusy and kCallRejected)
    RTCMD_CALL_REQ_CANCEL = 3,  // caller cancels the call requests, specifies the request id
    RTCMD_CALL_TERMINATE = 4, // hangup existing call, cancel call request. Works on an existing call
    RTCMD_JOIN = 5, // join an existing/just initiated call. There is no call yet, so the command identifies a call request
    RTCMD_SESSION = 6, // join was accepter and the receiver created a session to joiner
    RTCMD_SDP_OFFER = 7, // joiner sends an SDP offer
    RTCMD_SDP_ANSWER = 8, // joinee answers with SDP answer
    RTCMD_ICE_CANDIDATE = 9, // both parties exchange ICE candidates
    RTCMD_SESS_TERMINATE = 10, // initiate termination of a session
    RTCMD_SESS_TERMINATE_ACK = 11, // acknowledge the receipt of SESS_TERMINATE, so the sender can safely stop the stream and
    // it will not be detected as an error by the receiver
    RTCMD_MUTE = 12
};
enum TermCode: uint8_t
{
    kUserHangup = 0,         // < Normal user hangup
    kCallReqCancel = 1,      // < Call request was canceled before call was answered
    kCallRejected = 2,       // < Outgoing call has been rejected by the peer OR incoming call has been rejected by
    // <another client of our user
    kAnsElsewhere = 3,       // < Call was answered on another device of ours
    kAnswerTimeout = 5,      // < Call was not answered in a timely manner
    kRingOutTimeout = 6,     // < We have sent a call request but no RINGING received within this timeout - no other
    // < users are online
    kAppTerminating = 7,     // < The application is terminating
    kCallGone = 8,
    kBusy = 9,               // < Peer is in another call
    kNormalHangupLast = 20,  // < Last enum specifying a normal call termination
    kErrorFirst = 21,        // < First enum specifying call termination due to error
    kErrApiTimeout = 22,     // < Mega API timed out on some request (usually for RSA keys)
    kErrFprVerifFailed = 23, // < Peer DTLS-SRTP fingerprint verification failed, posible MiTM attack
    kErrProtoTimeout = 24,   // < Protocol timeout - one if the peers did not send something that was expected,
                             // < in a timely manner
    kErrProtocol = 25,       // < General protocol error
    kErrInternal = 26,       // < Internal error in the client
    kErrLocalMedia = 27,     // < Error getting media from mic/camera
    kErrNoMedia = 28,        // < There is no media to be exchanged - both sides don't have audio/video to send
    kErrNetSignalling = 29,  // < chatd shard was disconnected
    kErrIceDisconn = 30,     // < ice-disconnect condition on webrtc connection
    kErrIceFail = 31,        // <ice-fail condition on webrtc connection
    kErrSdp = 32,            // < error generating or setting SDP description
    kErrUserOffline = 33,    // < we received a notification that that user went offline
    kErrorLast = 33,         // < Last enum indicating call termination due to error
    kLast = 33,              // < Last call terminate enum value
    kPeer = 128,             // < If this flag is set, the condition specified by the code happened at the peer,
                             // < not at our side
    kInvalid = 0x7f
};

bool isTermError(TermCode code)
{
    return (code & 0x7f) >= TermCode::kErrorFirst;
}
const char* termCodeToStr(TermCode code);

class ISessionHandler
{
public:
    virtual void onStateChange(uint8_t newState) = 0;
    virtual void onDestroy(TermCode reason, bool byPeer, const std::string& msg) = 0;
    virtual void onRemoteStreamAdded(IVideoRenderer*& rendererOut) = 0;
    virtual void onRemoteStreamRemoved() = 0;
    virtual void onPeerMute(karere::AvFlags av) = 0;
};

class ICallHandler
{
public:
    virtual void onStateChange(uint8_t newState) {}
    virtual void onDestroy(TermCode reason, bool byPeer, std::string& msg) = 0;
    virtual ISessionHandler* onNewSession(ISession& sess) { return nullptr; }
    virtual void onLocalMediaError(const std::string errors) {}
    virtual void onRingOut(karere::Id peer) {}
    virtual void onCallStarting() {}
    virtual void onCallStarted() {}
};
class IGlobalHandler
{
public:
    virtual ICallHandler* onCallIncoming(ICall& call) = 0;
    virtual bool onAnotherCall(ICall& existingCall, karere::Id userid) = 0;
    virtual bool isGroupChat(karere::Id chatid) = 0;
};

class ISession: public karere::DeleteTrackable
{
protected:
    Call& mCall;
    karere::Id mSid;
    uint8_t mState;
    bool mIsJoiner;
    karere::Id mPeer;
    karere::Id mPeerAnonId;
    uint32_t mPeerClient;
    karere::AvFlags mPeerAv;
    ISession(Call& call, karere::Id peer, uint32_t peerClient): mCall(call), mPeer(peer), mPeerClient(peerClient){}
public:
    enum: uint8_t
    {
        kStateWaitSdpOffer, // < Session just created, waiting for SDP offer from initiator
        kStateWaitSdpAnswer, // < SDP offer has been sent by initiator, waniting for SDP answer
        kStateWaitLocalSdpAnswer, // < Remote SDP offer has been set, and we are generating SDP answer
        kStateInProgress,
        kStateTerminating, // < Session is in terminate handshake
        kStateDestroyed // < Session object is not valid anymore
    };
    static const char* stateToStr(uint8_t state);
    const char* stateStr() const { return stateToStr(mState); }
    bool isCaller() const { return !mIsJoiner; }
    Call& call() const { return mCall; }
    karere::Id peerAnonId() const { return mPeerAnonId; }
    virtual bool isRelayed() const;
};

class ICall: public karere::WeakReferenceable<ICall>
{
public:
    enum: uint8_t
    {
        kStateInitial,      // < Call object was initialised
        kStateHasLocalStream,
        kStateReqSent,      // < Call request sent
        kStateRingIn,       // < Call request received, ringing
        kStateJoining,      // < Joining a call
        kStateInProgress,
        kStateTerminating, // < Call is waiting for sessions to terminate
        kStateDestroyed    // < Call object is not valid anymore
    };
    static const char* stateToStr(uint8_t state);
    const char* stateStr() const { return stateToStr(mState); }
protected:
    RtcModule& mManager;
    karere::Id mChatid;
    chatd::Connection& mShard;
    karere::Id mId;
    uint8_t mState;
    bool mIsGroup;
    bool mIsJoiner;
    ICallHandler* mHandler;
    karere::Id mCallerUser;
    uint32_t mCallerClient;
    ICall(RtcModule& rtcModule, karere::Id chatid, chatd::Connection& shard,
        karere::Id callid, bool isGroup, bool isJoiner, ICallHandler* handler,
        karere::Id callerUser, uint32_t callerClient)
    : WeakReferenceable<ICall>(this), mManager(rtcModule), mChatid(chatid),
      mShard(shard), mId(callid), mIsGroup(isGroup), mIsJoiner(isJoiner),
      mHandler(handler), mCallerUser(callerUser), mCallerClient(callerClient)
    {}
public:
    chatd::Connection& shard() const { return mShard; }
    RtcModule& manager() const { return mManager; }
    uint8_t state() const { return mState; }
    karere::Id id() const { return mId; }
    karere::Id chatid() const { return mChatid; }
    bool isCaller() const { return !mIsJoiner; }
    karere::Id caller() const { return mCallerUser; }
    uint32_t callerClient() const { return mCallerClient; }
    void changeHandler(ICallHandler* handler) { mHandler = handler; }
    virtual karere::AvFlags sentAv() const;
    virtual karere::AvFlags receivedAv() const;
    virtual void hangup(TermCode reason=TermCode::kInvalid);
    virtual bool answer(karere::AvFlags av);
    virtual void changeLocalRenderer(IVideoRenderer* renderer);
    virtual karere::AvFlags muteUnmute(karere::AvFlags av);
    virtual karere::AvFlags localAv() const;
};
struct SdpKey
{
    uint8_t data[32];
};

struct VidEncParams
{
    /** @brief Minimum bitrate for video encoding, in kbits/s */
    uint16_t minBitrate = 0;
    /** @brief Maximum bitrate for video encoding, in kbits/s */
    uint16_t maxBitrate = 0;
    /** @brief Maximum spatial quantization for video encoding.
      * This specifies the maximum size of image area to be encoded with one
      * color - the bigger this value is, the more coarse and blocky the image
      * is. Value of 1 should disable quantization.
      */
    uint16_t maxQuant = 0;

    /** @brief The target buffer latency of the video stream, in milliseconds */
    uint16_t bufLatency = 0;

};

/** @brief This is the public interface of the RtcModule */
class IRtcModule: public karere::DeleteTrackable
{
protected:
    karere::Client& mClient;
    IGlobalHandler* mHandler;
    IRtcCrypto& mCrypto;
    karere::Id mOwnAnonId;
    std::string mVideoInDeviceName;
    std::string mAudioInDeviceName;
    IRtcModule(karere::Client& client, IGlobalHandler* handler, IRtcCrypto& crypto,
        karere::Id ownAnonId)
        :mClient(client), mHandler(handler), mCrypto(crypto), mOwnAnonId(ownAnonId) {}
public:
    virtual IRtcModule* create(karere::Client& client, IGlobalHandler* handler,
        IRtcCrypto& crypto, const karere::ServerList<karere::TurnServerInfo>& iceServers);
    /** @brief Default video encoding parameters. */
    VidEncParams vidEncParams;
    karere::Id ownAnonId() const { return mOwnAnonId; }

    /** @brief Returns a list of all detected audio input devices on the system */
    virtual void getAudioInDevices(std::vector<std::string>& devices) const = 0;

    /** @brief Returns a list of all detected video input devices on the system */
    virtual void getVideoInDevices(std::vector<std::string>& devices) const = 0;

    /** @brief Selects a video input device to be used for subsequent calls. This can be
     * changed just before a call is made, to allow different calls to use different
     * devices
     * @returns \c true if the specified device was successfully selected,
     * \c false if a device with that name does not exist or could not be selected
     */
    virtual bool selectVideoInDevice(const std::string& devname) = 0;

    /** @brief Selects an audio input device to be used for subsequent calls. This can be
     * changed just before a call is made, to allow different calls to use different
     * devices
     * @returns \c true if the specified device was successfully selected,
     * \c false if a device with that name does not exist or could not be selected
     */
    virtual bool selectAudioInDevice(const std::string& devname) = 0;

    /**
     * @brief Initiates a call to the specified JID.
     * @param userHandler - the event handler interface that will receive further events
     * about the call
     * @param targetJid - the bare or full JID of the callee. If the JID is bare,
     * the call request is broadcasted to all devices of that user. If the JID is
     * full (includes the xmpp resource), then only that device will receive the call request.
     * @param av What streams to send - audio and/or video or none.
     * @param files - used for file transfer calls, not implemented yet.
     * @param myJid - used when in a XMPP conference chatroom to specify our room-specific jid
     */
    virtual bool isCaptureActive() const = 0;

    virtual std::shared_ptr<ICall> joinCall(karere::Id chatid, karere::AvFlags av, ICallHandler* handler);
    virtual std::shared_ptr<ICall> startCall(karere::Id chatid, karere::AvFlags av, ICallHandler* handler);
    virtual void hangupAll() {}
};
}

#endif
#endif
