#include "strophe.jingle.h"
#include "streamPlayer.h"

#define RTCM_EVENT(name,...) \
    printf("Event: %s\n", #name); \
    mEventHandler->name(#__VA_ARGS__)

namespace karere
{
namespace rtcModule
{
/** This is the class that implements the user-accessible API to webrtc.
 * Since the rtc module may be built by a different compiler (necessary on windows),
 * it is built as a shared object and its API is exposed via virtual interfaces.
 * This means that any public API is implemented via virtual methods in this class
*/
using namespace promise;
using namespace strophe;
struct AvTrackBundle
{
    rtc::scoped_refptr<AudioTrackInterface> audio;
    rtc::scoped_refptr<AudioTrackInterface> video;
};
//Public interfaces
//TODO: move to public header
/** Interface of object passed to onIncomingCallRequest. This interface contains methods
 *  to get info about the call request, has a method to answer or reject the call.
 */
struct IAnswerCall
{
    virtual int answer(bool accept, const AvFlags& answerAv,
                       const char* reason, const char* text) = 0;
    virtual const char** files() const = 0;
    virtual const AvFlags& peerAv() const = 0;
    virtual bool reqStillValid() const = 0;
    virtual void destroy() { delete this; }
private:
    virtual ~IAnswerCall() {} //deleted via destoy() only to use our memory manager
};

/** Public event handler callback interface.
 *  All events from the Rtc module are sent to the application via this interface */
struct IEventHandler
{
};
//===
/** Before being answered/rejected, initiated calls (via startMediaCall) are put in a map
 * to allow hangup() to operate seamlessly on calls in all states.These states are:
 *  - requested by us but not yet answered(mCallRequests map)
 *  - requested by peer but not yet initiated (mAutoAcceptCalls map)
 *  - in progress (mSessions map)
 */
struct CallRequest
{
    std::string targetJid;
    bool isFileTransfer;
    std::function<bool()> cancel;
    CallRequest(const std::string& aTargetJid, bool aIsFt, std::function<bool()>&& cancelFunc)
        : targetJid(aTargetJid), isFileTransfer(aIsFt), cancel(cancelFunc){}
};

string avFlagsToString(const AvFlags& av);

class RtcHandler: public Jingle
{
protected:
    typedef Jingle Base;
    AvTrackBundle mLocalTracks;
    std::shared_ptr<artc::StreamPlayer> mLocalVid;
    IEventHandler* mEventHandler;
    std::map<std::string, std::shared_ptr<CallRequest> > mCallRequests;
public:
    RtcHandler(strophe::Connection& conn, IEventHandler* handler,
               ICryptoFunctions* crypto, const std::string& iceServers)
        :Jingle(conn, crypto, servers), mEventHandler(handler)
    {
 //   if (RTC.browser == 'firefox')
 //       this.jingle.media_constraints.mandatory.MozDontOfferDataChannel = true;
    }
protected:
bool hasLocalStream()
{
    return (mLocalTracks.audio || mLocalTracks.video);
}
void logInputDevices()
{
    auto& devices = mDeviceManager.inputDevices();
    KR_LOG_DEBUG("Input devices on this system:");
    for (const auto& dev: devices.audio)
        KR_LOG_DEBUG("\tAudio: %s\n", dev.name.c_str());
    for (const auto& dev: devices.video)
        KR_LOG_DEBUG("\tVideo: %s\n", dev.name.c_str());
}

string getLocalAudioAndVideo()
{
    string errors;
    if (hasLocalStream())
        throw runtime_error("getLocalAudioAndVideo: Already has tracks");
    const auto& devices = deviceManager.inputDevices();
    if (devices.video.size() > 0)
      try
        {
             mLocalTracks.video = deviceManager.getUserVideo(
                          artc::MediaGetOptions(devices.video[0]));
        }
        catch(exception& e)
        {
            errors.append("Error getting video device: ")
                  .append(e.what()?e.what():"Unknown error")+='\n';
        }

    if (devices.audio.size() > 0)
        try
        {
            mLocalTracks.audio = deviceManager.getUserAudio(
                          artc::MediaGetOptions(devices.audio[0]));
        }
        catch(exception& e)
        {
            errors.append("Error getting audio device: ")
                  .append(e.what()?e.what():"Unknown error")+='\n';
        }
    return errors;
}
template <class OkCb, class ErrCb>
void myGetUserMedia(const AvFlags& av, OkCb okCb, ErrCb errCb, bool allowEmpty=false)
{
    try
    {
        bool lmfCalled = false;
        string errors;
        bool alreadyHadStream = hasLocalStream();
        if (!alreadyHadStream)
        {
            errors = getLocalAudioAndVideo(errors);
            mLocalStreamRefCount = 0;
        }
        auto stream = artc::gWebrtcContext->CreateLocalMediaStream("localStream");
        if(!stream.get())
        {
            string msg = "Could not create local stream: CreateLocalMediaStream() failed";
            lmfCalled = true;
            RTCM_EVENT(onLocalMediaFail, msg.c_str());
            return errCb(msg);
        }

        if (!hasLocalStream() || !errors.empty())
        {
            string msg = "Error getting local stream track(s)";
            if (!errors.empty())
                msg.append(": ").append(errors);

            if (!allowEmpty)
            {
                lmfCalled = true;
                RTCM_EVENT(onLocalMediaFail, msg.c_str());
                return errCb(msg);
            }
            else
            {
                lmfCalled = true;
                bool cont = false;
                RTCM_EVENT(onLocalMediaFail, msg.c_str(), &cont);
                if (!cont)
                    return errCb(msg);
            }
        }

        if (mLocalTracks.audio)
            KR_THROW_IF_FALSE(stream->AddTrack(
                              deviceManager.cloneAudioTrack(mLocalTracks.audio)));
        if (mLocalTracks.video)
            KR_THROW_IF_FALSE(stream->AddTrack(
                              deviceManager.cloneVideoTrack(mLocalTracks.video)));

        if (!alreadyHadStream)
            onLocalStreamReady(); //creates local player but does not link it to stream
        refLocalStream(av.video); //links player to stream
        okCb(stream);
    }
    catch(exception& e)
    {
        if (!lmfCalled)
            RTCM_EVENT(onLocalMediaFail, e.what());
        return errCb(e.what()?e.what():"Error getting local media stream");
    }
}
 
void onConnState(const xmpp_conn_event_t status,
            const int error, xmpp_stream_error_t * const stream_error)
{
    Base::onConnState(status, error, stream_error);
    switch (status)
    {
        case XMPP_CONN_FAIL:
        case XMPP_CONN_DISCONNECT:
        {
            terminateAll('disconnected', null, true); //TODO: Maybe move to Jingle?
            freeLocalStreamIfUnused();
            break;
        }
        case XMPP_CONN_CONNECT:
        {
            mConn.addHandler(std::bind(&RtcHandler::onPresenceUnavailable, this, _2),
               NULL, "presence", "unavailable");
            break;
        }
    }
 }

/**
    Initiates a media call to the specified peer
    @param {string} targetJid
        The JID of the callee. Can be a full jid (including resource),
        or a bare JID (without resource), in which case the call request will be broadcast
        using a special <message> packet. For more details on the call broadcast mechanism,
        see the Wiki
    @param {MediaOptions} options Call options
    @param {boolean} options.audio Send audio
    @param {boolean} options.video Send video
    @param {string} [myJid]
        Necessary only if doing MUC, because the user's JID in the
        room is different than her normal JID. If not specified,
        the user's 'normal' JID will be used
    @returns {{cancel: function()}}
        Returns an object with a cancel() method, that, when called, cancels the call request.
        This method returns <i>true</i> if the call was successfully canceled, and <i>false</i>
        in case the call was already answered by someone.
*/

int startMediaCall(char* sidOut, const char* targetJid, const AvFlags& av, const char* files[]=NULL,
                      const char* myJid=NULL)
{
  if (!sidOut || !targetJid)
      return RTCM_EPARAM;
  struct State;
  {
      string sid;
      string targetJid;
      string ownFprMacKey;
      string myJid;
      xmpp_handler ansHandler = nullptr;
      xmpp_handler declineHandler = nullptr;
      int state = 0; //state 0 means not yet got usermedia, 1 means got user media and waiting for peer, state 2 means pear answered or timed out, 4 means call was canceled by us via the cancel() method of the returned object
      artc::tspMediaStream sessStream;
  };
  shared_ptr<State> state(new State);
  bool isBroadcast = strophe::getResourceFromJid(targetJid).empty();
  state->ownFprMacKey = crypto.generateFprMacKey();
  state->sid = crypto().generateRandomString(RTCM_SESSIONID_LEN);
  state->myJid = myJid?myJid:"";
  state->targetJid = targetJid;
  var fileArr;
  auto initiateCallback = [this, state, files](artc::tspMediaStream sessStream)
  {
      auto actualAv = getStreamAv(sessStream);
      if ((av.audio && !actualAv.audio) || (av.video && !actualAv.video))
      {
          KR_LOG_WARNING("startMediaCall: Could not obtain audio or video stream requested by the user");
          av.audio = actualAv.audio;
          av.video = actualAv.video;
      }
      if (state->state == 4)
      {//call was canceled before we got user media
          freeLocalStreamIfUnused();
          return;
      }
      state->state = 1;
      state->sessStream = sessStream;
// Call accepted handler
      state->ansHandler = mConn.addHandler([this, state](Stanza stanza, void*, bool& keep)
      {
          try
          {
              if (stanza.attr("sid") != state->sid)
                  return; //message not for us, keep handler(by default keep==true)

              keep = false;
              mCallRequests.erase(state->sid);

              if (state->state !== 1)
                  return;
              state->state = 2;
              mConn.removeHandler(state->declineHandler);
              state->declineHandler = nullptr;
              state->ansHandler = nullptr;
// The crypto exceptions thrown here will simply discard the call request and remove the handler
              string peerFprMacKey = stanza.attr("fprmackey");
              try
              {
                  peerFprMacKey = crypto().decryptMessage(peerFprMacKey);
                  if (peerFprMacKey.empty())
                      peerFprMacKey = crypto().generateFprMacKey();
              }
              catch(e)
              {
                  peerFprMacKey = crypto().generateFprMacKey();
              }
              const char* peerAnonId = stanza.attr("anonid");
              if (peerAnonId[0] == 0) //empty
                  throw runtime_error("Empty anonId in peer's call answer stanza");
              const char* fullPeerJid = stanza.attr("from");
              if (state->isBroadcast)
              {
                  Stanza msg(mConn);
                  msg.setName("message")
                     .setAttr("to", strophe::getBareJidFromJid(state->targetJid))
                     .setAttr("type", "megaNotifyCallHandled")
                     .setAttr("sid", state->sid.c_str())
                     .setAttr("by", fullPeerJid)
                     .setAttr("accepted", "1");
                  xmpp_send(mConn, msg);
              }

              JingleSession* sess = initiate(state->sic().c_str(), fullPeerJid,
                  myJid?myJid:mConn.jid(), sessStream,
                  sessStream?avFlagsToMutedState(av, sessStream):AvFlags(), {
                      {"ownFprMacKey", state->ownFprMacKey.c_str()},
                      {"peerFprMacKey", peerFprMacKey},
                      {"peerAnonId", peerAnonId}
                  });//,
                 // files?
                 // ftManager.createUploadHandler(sid, fullPeerJid, fileArr):NULL);

              RTCM_EVENT(onCallInit, sess, !!files);
        }
        catch(runtime_error& e)
        {
          freeLocalStreamIfUnused();
          KR_LOG_ERROR("Exception in call answer handler:\n%s\nIgnoring call", e.what());
        }
      }, NULL, "message", "megaCallAnswer", NULL, targetJid, STROPHE_MATCH_BAREJID);

//Call declined handler
      state->declineHandler = mConn.addHandler([this, state](Stanza stanza, void*, bool& keep)
      {
          if (stanza.attr("sid") != sid) //this message was not for us
              return;

          keep = false;
          mCallRequests.erase(state->sid);

          if (state->state != 1)
              return;

          state->state = 2;
          mConn.removeHandler(state->ansHandler);
          state->ansHandler = nullptr;
          state->declineHandler = nullptr;
          state->sessStream = nullptr;
          freeLocalStreamIfUnused();

          string text;
          Stanza body = stanza.child("body", true);
          if (body)
          {
              const char* txt = body.textOrNull();
              if (txt)
                  text = xmlUnescape(txt);
          }
          const char* fullPeerJid = stanza.attr("from");

          if (isBroadcast)
          {
              Stanza msg(mConn);
              msg.setName("message")
                 .setAttr("to", getBareJidFromJid(state->targetJid.c_str()))
                 .setAttr("type", "megaNotifyCallHandled")
                 .setAttr("sid", state->sid.c_str())
                 .setAttr("by", fullPeerJid)
                 .setAttr("accepted", "0");
              xmpp_send(mConn, msg);
          }
          RTCM_EVENT(oncallDeclined,
              fullPeerJid, //peer
              state->sid.c_str(),
              stanza.attrOrNull("reason"),
              text.empty()?nullptr:text.c_str(),
              !!files, //isDataCall
          );
      },
      nullptr, "message", "megaCallDecline", nullptr, targetJid, STROPHE_MATCH_BAREJID);

      crypto().preloadCryptoForJid([this, state, av]()
      {
          Stanza msg(mConn);
          msg.setName("message")
             .setAttr("to", state->targetJid.c_str())
             .setAttr("type", "megaCall")
             .setAttr("sid", state->sid.c_str())
             .setAttr("fprmackey", crypto().encryptMessageForJid(state->ownFprMacKey, state->targetJid))
             .setAttr("anonid", mOwnAnonId);
/*          if (files)
          {
              var infos = {};
              fileArr = [];
              var uidPrefix = sid+Date.now();
              var rndArr = new Uint32Array(4);
              crypto.getRandomValues(rndArr);
              for (var i=0; i<4; i++)
                  uidPrefix+=rndArr[i].toString(32);
              for (var i=0; i<options.files.length; i++) {
                  var file = options.files[i];
                  file.uniqueId = uidPrefix+file.name+file.size;
                  fileArr.push(file);
                  var info = {size: file.size, mime: file.type, uniqueId: file.uniqueId};
                  infos[file.name] = info;
              }
              msgattrs.files = JSON.stringify(infos);
          } else {
 */
          msg.setAttr("media", avFlagsToString(av).c_str());
          xmpp_send(mConn, msg);
      }, targetJid);

      if (!files)
          setTimeout([this, state]()
          {
              mCallRequests.erase(state->sid);
              if (state->state != 1)
                  return;

              state->state = 2;
              mConn.deleteHandler(state->ansHandler);
              state->ansHandler = nullptr;
              mConn.deleteHandler(state->declineHandler);
              state->declineHandler = nullptr;
              state->sessStream = nullptr;
              freeLocalStreamIfUnused();
              Stanza cancelMsg(mConn);
              cancelMsg.setName("message")
                      .setAttr("type", "megaCallCancel")
                      .setAttr("to", getBareJidFromJid(state->targetJid.c_str()));
              xmpp_send(mConn, cancelMsg);
              RTCM_EVENT(onCallAnswerTimeout, state->targetJid.c_str());
          },
          callAnswerTimeout);
  }; //end initiateCallback()
  if (av.audio || av.video)
      myGetUserMedia(av, initiateCallback, nullptr, true);
  else
      initiateCallback(nullptr);

  //return an object with a cancel() method
  if (mCallRequests.find(state->sid) != mCallRequests.end())
      throw runtime_error("Assert failed: There is already a call request with this sid");
  mCallRequests.emplace(sid, state->targetJid, !!files, [this, state]()
  {
      mCallRequests.erase(state->sid);
      if (state->state == 2)
          return false;
      if (state->state == 1)
      { //same as if (ansHandler)
            state->state = 4;
            mConn.removeHandler(state->ansHandler);
            state->ansHandler = nullptr;
            mConn.removeHandler(state->declineHandler);
            state->declineHandler = nullptr;

            freeLocalStreamIfUnused();
            Stanza cancelMsg(mConn);
            cancelMsg.setName("message")
                    .setAttr("to", getBareJidFromJid(state->targetJid).c_str())
                    .setAttr("sid", state->sid.c_str())
                    .setAttr("type", "megaCallCancel");
            xmpp_send(mConn, cancelMsg);
            return true;
      }
      else if (state == 0)
      {
          state = 4;
          return true;
      }
      KR_LOG_WARNING("RtcSession: BUG: cancel() called when state has an unexpected value of", state->state);
      return false;
  };
  strncpy(sidOut, state->sid.c_str(), RTCM_SESSIONID_LEN);
  return 0;
}
template <class CB>
void enumCallsForHangup(CB cb, const char* reason, const char* text)
{
    for (auto callReq=mCallRequests.begin(); callReq!=mCallRequests.end();)
    {
        if (cb(callReq->first, callReq->second.targetJid,
               1|(callReq->second.isFileTransfer?0x0100:0)));
        {
            auto erased = callReq;
            callReq++;
            erased->second.cancel(); //erases 'erased'
        }
        else
        {
            callReq++;
        }
    }
    for (auto ans=mAutoAcceptCalls.begin(); ans!=mAutoAcceptCalls.end();)
    {
        if (cb(ans->first, ans->second->at("from"),
               2|(ans->secons.ftHandler?0x0100:0)))
        {
            auto erased = ans;
            ans++;
            cancelAutoAcceptEntry(erased, reason, text);
        }
        else
        {
            ans++;
        }
    }
    for (auto sess=mSessions.begin(); sess!=mSessions.end();)
    {
        if (cb(sess->first, sess->second->peerJid(),
               3|(sess->second->ftHandler()?0x0100:0)))
        {
            auto erased = sess;
            sess++;
            terminateBySid(erased->first, reason, text);
        }
        else
        {
            sess++;
        }
    }
}
inline bool callTypeMatches(int type, char userType)
{
    bool isFtCall = (type & 0x0100);
    return (((userType == 'm') && !isFtCall) ||
            ((userType == 'f') && isFtCall));
}

bool hangupBySid(const char* sid, char callType, const char* reason, const char* text)
{
    bool term = false;
    enumCallsForHangup([sid, &term, callType](const string& aSid, const string& peer, int type)
    {
        if (!callTypeMatches(type, callType))
            return false;

        if (aSid == sid)
        {
            if (term)
                KR_LOW_WARNING("hangupBySid: BUG: Already terminated one call with that sid");
            term = true;
            return true;
        }
        else
        {
            return false;
        }
    },
    reason, text);
    return term;
}
bool hangupByPeer(const char* peerJid, char callType, const char* reason, const char* text)
{
    bool term = false;
    bool isBareJid = !!strchr(peerJid, '/');
    if (isBareJid)
        enumCallsForHangup([peerJid, callType, &term](const string& aSid, const string& peer, int type)
        {
            if (!callTypeMatches(type, callType))
                return false;
            if (getBareJidFromJid(peer) == peerJid)
            {
                term = true;
                return 1;
            }
            else
            {
                return false;
            }
        }, reason, text);
    else
        enumCallsForHangup([peerJid, &term](const string& aSid, const string& peer, int type)
        {
            if (!callTypeMatches(type, callType))
                return false;
            if (peer == peerJid)
            {
                term = true;
                return true;
            }
            else
            {
                return false;
            }
        }, reason, text);
    return term;
}

bool hangupAll(const char* reason, const char* text)
{
    bool term = false;
    enumCallsForHangup([&term](const string&, const string&, int)
    {
        term = true;
        return true;
    },
    reason, text);
    return term;
}

 /**
    Mutes/unmutes audio/video
    @param {boolean} state
        Specifies whether to mute or unmute:
        <i>true</i> mutes,  <i>false</i> unmutes.
    @param {object} what
        Determine whether the (un)mute operation applies to audio and/or video channels
        @param {boolean} [what.audio] The (un)mute operation is applied to the audio channel
        @param {boolean} [what.video] The (un)mute operation is applied to the video channel
    @param {string} [jid]
        If given, specifies that the mute operation will apply only
        to the call to the given JID. If not specified,
        the (un)mute will be applied to all ongoing calls.
 */
void muteUnmute(bool state, const AvFlags& what, const char* jid)
{
    int affected = 0;
    if (jid)
    {
        bool isBareJid = !!strchr(jid, '/');
        for (auto& sess: mSessions)
        {
            const auto& peer = sess.second->peerJid();
            bool match = isBareJid?(jid==getBareJidFromJid(peer)):(jid==peer);
            if (!match)
                continue;
            affected++;
            sess.second->muteUnmute(state, what);
        }
    }
// If we are muting all calls, disable also local video playback as well
// In Firefox, all local streams are only references to gLocalStream, so muting any of them
// mutes all and the local video playback.
// In Chrome all local streams are independent, so the local video stream has to be
// muted explicitly as well
    if (what.video && (!jid || (affected >= mSessions.size())))
    {
        if (state)
            disableLocalVid();
        else
            enableLocalVid();
    }
}

void onPresenceUnavailable(Stanza pres)
{
    const char* from = pres.attr("from");
    for (auto sess = mSessions.begin(); sess!=mSessions.end();)
    {
        if (sess.second->peerJid() == from)
        {
            auto erased = sess;
            sess++;
            terminate(erased->second, 'peer-disconnected');
        }
        else
        {
            sess++;
        }
    }
}

void createLocalPlayer()
{
// This is called by myGetUserMedia when the the local stream is obtained (was not open before)
    if (mLocalVid)
        throw new Error("Local stream just obtained, but localVid was not NULL");
    mLocalVid = new artc::StreamPlayer(requestLocalVideoRenderer(), nullptr, nullptr);
//    RTCM_EVENT(onLocalStreamObtained); //TODO: Maybe provide some interface to the player, but must be virtual because it crosses the module boundary
    maybeCreateVolMon();
}

struct AnswerCallController: public IAnswerCall
{
    RtcHandler& self;
    shared_ptr<CallAnswerFunc> mAnsFunc;
    shared_ptr<function<bool(void)> > mReqStillValid;
    AvFlags mPeerAv;
    shared_ptr<set<string> > mFiles;
    vector<const char*> mFileCStrings;

    AnswerCallController(RtcHandler& aSelf, shared_ptr<CallAnswerFunc>& ansFunc,
      shared_ptr<function<void()> >& aReqStillValid, AvFlags& aPeerAv,
      shared_ptr<set<string> >& aFiles)
    :self(aSelf), mAnsFunc(ansFunc),
      mReqStillValid(aReqStillValid), mPeerAv(aPeerAv), mFiles(aFiles),

    {
        if(mFiles)
        {
            for(auto& f: *mFiles)
                mFileCStrings.push_back(f.c_str());
            mFileCStrings.push_back(nullptr);
        }
    }
    virtual const char* files() const
    {
        if (!mFiles)
            return nullptr;
        else
            return &(mFileCStrings[0]);
    }
    virtual const AvFlags& peerAv() const {return mPeerAv;}
    virtual bool reqStillValid() const {return mReqStillValid();}
    virtual int answer(bool accept, const AvFlags &answerAv,
        const char *reason, const char *text)
    {
        if (!accept)
            return (*mAnsFunc)(false, nullptr, reason?reason:"busy", text)?0:ECANCELED;
        if (!(*reqStillValid)())
            return ECANCELED;
        if (!mFiles)
        {
            self.myGetUserMedia(answerAv,
               [mAnsFunc](artc::tspMediaStream sessStream)
            {
                AnswerOptions* opts = new AnswerOptions;
                opts->localStream = sessStream;
                opts->muted = avFlagsToMutedState(answerAv, sessStream); //TODO: Are these reverse?
                (*ansFunc)(true, opts, nullptr, nullptr);
            },
            [ansFunc](const string& err)
            {
                ansFunc(false, "error", ("There was a problem accessing user's camera or microphone. Error: "+err).c_str());
            },
            //allow to answer with no media only if peer is sending something
            (mPeerAv.audio || mPeerAv.video));
        }
        else
        {//file transfer
            ansFunc(true, nullptr, nullptr, nullptr);
        }
      return 0;
    }
};

virtual void onIncomingCallRequest(const char* from, shared_ptr<CallAnswerFunc>& ansFunc,
    shared_ptr<function<bool()> >& reqStillValid, const AvFlags& peerMedia,
    shared_ptr<set<string> >& files)
{
    //this is the C cross-module answer function that the application calls to answer or reject the call
    AnswerCallController* ansCtrl = new AnswerCallController(
                *this, ansFunc, reqStillValid, peerMedia, files);
    RTCM_EVENT(onCallIncomingRequest, ansCtrl);
}
void onCallAnswered(JingleSession& sess)
{
    RTCM_EVENT(onCallAnswered, sess);
}

void removeRemotePlayer(JingleSession& sess)
{
    /**
        The media session with peer JID has been destroyed, and the video element
            has to be removed from the DOM.
        @event "remote-player-remove"
        @type object
        @property {string} id The id of the html video element to be removed
        @property {string} peer The full jid of the peer
        @property {SessWrapper} sess
    */
    if (!sess.remotePlayer)
    {
        KR_LOG_ERROR("removeVideo: remote player is already NULL");
        return;
    }
    sess.remotePlayer.stop();
    RTCM_EVENT(remotePlayerRemove, sess);
    sess.remotePlayer.reset(nullptr);
}

void onMediaRecv(playerElem, sess, stream) {
    if (!this.jingle.sessionIsValid(sess)) {
        this.error("onMediaRecv received for non-existing session:", sid)
        return;
    }
 /**
    Triggered when actual media packets start being received from <i>peer</i>,
    on session <i>sess</i>. The video DOM element has just been created, and is passed as the
    player property.
    @event "media-recv"
    @type {object}
    @property {string} peer The full JID of the peer
    @property {SessWrapper} sess The session
    @property {MediaStream} stream The remote media stream
    @property {DOM} player
    The video player element that has just been created for the remote stream.
    The element will always have the rmtViewport CSS class.
    If there is no video received, but only audio, the element will have
    also the rmtNoVideo CSS class.
    <br>NOTE: Because video is always negotiated if there is a camera, even if it is not sent,
    the rmt(No)Video is useful only when the peer does not have a camera at all,
    and is not possible to start sending video later during the call (for desktop
    sharing, the call has to be re-established)
 */
    var obj = {peer: sess.peerjid, sess:new SessWrapper(sess), stream: stream, player: playerElem};
    this.trigger('media-recv', obj);
    if (obj.stats || this.statsUrl) {
        if (typeof obj.stats !== 'object')
            obj.stats = {scanPeriod: 1, maxSamplePeriod: 5};

        if (RTC.Stats) {
            var s = obj.stats;
            if (!s.scanPeriod || !s.maxSamplePeriod)
                return;
            sess.statsRecorder = new RTC.Stats.Recorder(sess, s.scanPeriod, s.maxSamplePeriod, s.onSample);
            sess.statsRecorder.start();
        }
        else
            console.warn("RTC stats requested, but stats API not available on this browser");
    }
    
    sess.tsMediaStart = Date.now();
 },

 onCallTerminated: function(sess, reason, text) {
 try {
 //WARNING: sess may be a dummy object, only with peerjid property, in case something went
 //wrong before the actual session was created, e.g. if SRTP fingerprint verification failed

   /**
   Call was terminated, either by remote peer or by us
    @event "call-ended"
    @type {object}
    @property {string} peer The remote peer's full JID
    @property {SessWrapper} sess The session of the call
    @property {string} [reason] The reason for termination of the call
    @property {string} [text]
        The verbose reason or error message for termination of the call
    @property {object} [stats]
        The statistics gathered during the call, if stats were enabled
    @property {object} [basicStats]
        In case statistics are not available on that browser, or were not enabled,
        this property is set and contains minimum info about the call that can be
        used by a stats server
    @property {string} basicStats.callId
        The callId that the statistics engine would provide
    @property {number} basicStats.callDur
        The duration of actual media in seconds (ms rounded via Math.ceil()) that
        the stats engine would have provided
   */
    var obj = {
        peer: sess.peerjid,
        sess: new SessWrapper(sess), //can be a dummy session but the wrapper will still work
        reason:reason, text:text
    };
    if (sess.statsRecorder)  {
        var stats = obj.stats = sess.statsRecorder.terminate(this._makeCallId(sess));
        stats.isCaller = sess.isInitiator?1:0;
        stats.termRsn = reason;
    }
    else { //no stats, but will still provide callId and duration
        var bstats = obj.basicStats = {
            isCaller: sess.isInitiator?1:0,
            termRsn: reason,
            bws: stats_getBrowserVersion()
        };

        if (sess.fake) {
            sess.me = this.jid; //just in case someone wants to access the own jid of the fake session
            if (!sess.peerAnonId)
                sess.peerAnonId = "_unknown";
        }
        bstats.cid = this._makeCallId(sess);
        if (sess.tsMediaStart) {
            bstats.ts = Math.round(sess.tsMediaStart/1000);
            bstats.dur = Math.ceil((Date.now()-sess.tsMediaStart)/1000);
        }
    }
    if (this.statsUrl)
       jQuery.ajax(this.statsUrl, {
            type: 'POST',
            data: JSON.stringify(obj.stats||obj.basicStats)
    });
    this.trigger('call-ended', obj);
    if (!sess.fake) { //non-fake session
        if (sess.localStream)
            sess.localStream.stop();
        this.removeVideo(sess);
    }
    this._freeLocalStreamIfUnused();
 } catch(e) {
    console.error("onTerminate() handler threw an exception:\n", e.stack?e.stack:e);
 }
 },

 _freeLocalStreamIfUnused: function() {
     var sessions = this.jingle.sessions;
    for (var sess in sessions)
        if (sessions[sess].localStream) //in use
            return;

//last call ended
    this._unrefLocalStream();
 },

 waitForRemoteMedia: function(playerElem, sess) {
    if (!this.jingle.sessionIsValid(sess))
        return;
    var self = this;
    if (playerElem[0].currentTime > 0) {
        this.onMediaRecv(playerElem, sess, sess.remoteStream);
        RTC.attachMediaStream(playerElem, sess.remoteStream); // FIXME: why do i have to do this for FF?
       // console.log('waitForremotevideo', sess.peerconnection.iceConnectionState, sess.peerconnection.signalingState);
    }
    else
        setTimeout(function () { self.waitForRemoteMedia(playerElem, sess); }, 200);
 },
//onRemoteStreamAdded -> waitForRemoteMedia (waits till time>0) -> onMediaRecv() -> addVideo()
 onRemoteStreamAdded: function(sess, event) {
    if ($(document).find('#remotevideo_'+sess.sid).length !== 0) {
        console.warn('Ignoring duplicate onRemoteStreamAdded for session', sess.sid); // FF 20
        return;
    }
/**
    @event "remote-sdp-recv"
    @type {object}
    @property {string} peer The full JID of the peer
    @property {MediaStream} stream The remote media stream
    @property {SessWrapper} sess The call session
*/
    this.trigger('remote-sdp-recv', {peer: sess.peerjid, stream: event.stream, sess: new SessWrapper(sess)});
    var elemClass;
    var videoTracks = event.stream.getVideoTracks();
    if (!videoTracks || (videoTracks.length < 1))
        elemClass = 'rmtViewport rmtNoVideo';
    else
        elemClass = 'rmtViewport rmtVideo';

    this._attachRemoteStreamHandlers(event.stream);
    // after remote stream has been added, wait for ice to become connected
    // old code for compat with FF22 beta
    var elem = $("<video autoplay='autoplay' class='"+elemClass+"' id='remotevideo_" + sess.sid+"' />");
    RTC.attachMediaStream(elem, event.stream);
    this.waitForRemoteMedia(elem, sess); //also attaches media stream once time > 0

//     does not yet work for remote streams -- https://code.google.com/p/webrtc/issues/detail?id=861
//    var options = { interval:500 };
//    var speechEvents = hark(data.stream, options);

//    speechEvents.on('volume_change', function (volume, treshold) {
//      console.log('volume for ' + sid, volume, treshold);
//    });
 },

 onRemoteStreamRemoved: function(event) {
 },

 noStunCandidates: function() {
 },

 onJingleError: function(sess, err, stanza, orig) {
    if (err.source == 'transportinfo')
        err.source = 'transport-info (i.e. webrtc ice candidate)';
    if (!orig)
        orig = "(unknown)";

    if (err.isTimeout) {
        console.error('Timeout getting response to "'+err.source+'" packet, session:'+sess.sid+', orig-packet:\n', orig);
 /**
    @event "jingle-timeout"
    @type {object}
    @property {string} src A semantic name of the operation where the timeout occurred
    @property {DOM} orig The original XML packet to which the response timed out
    @property {SessWrapper} sess The session on which the timeout occurred
  */
        this.trigger('jingle-timeout', {src: err.source, orig: orig, sess: new SessWrapper(sess)});
    }
    else {
        if (!stanza)
            stanza = "(unknown)";
        console.error('Error response to "'+err.source+'" packet, session:', sess.sid,
            '\nerr-packet:\n', stanza, '\norig-packet:\n', orig);
 /**
    @event "jingle-error"
    @type {object}
    @property {string} src A semantic name of the operation where the error occurred
    @property {DOM} orig The XML stanza in response to which an error stanza was received
    @property {DOM} pkt The error XML stanza
    @property {SessWrapper} sess The session on which the error occurred
 */
        this.trigger('jingle-error', {src:err.source, pkt: stanza, orig: orig, sess: new SessWrapper(sess)});
    }
 },

 /**
    Get info whether local audio and video are being sent at the moment in a call to the specified JID
    @param {string} fullJid The <b>full</b> JID of the peer to whom there is an ongoing call
    @returns {{audio: Boolean, video: Boolean}} If there is no call to the specified JID, null is returned
 */
 getSentMediaTypes: function(fullJid)
 {
     var sess = this.getMediaSessionToJid(fullJid);
     if (!sess)
        return null;
//we don't use sess.mutedState because in Forefox we don't have a separate
//local streams for each session, so (un)muting one session's local stream applies to all
//other sessions, making mutedState out of sync
    var audTracks = sess.localStream.getAudioTracks();
    var vidTracks = sess.localStream.getVideoTracks();
    return {
        audio: (audTracks.length > 0) && audTracks[0].enabled,
        video: (vidTracks.length > 0) && vidTracks[0].enabled
    }
 },

 /**
    Get info whether remote audio and video are being received at the moment in a call to the specified JID
    @param {string} fullJid The full peer JID to identify the call
    @returns {{audio: Boolean, video: Boolean}} If there is no call to the specified JID, null is returned
 */
 getReceivedMediaTypes: function(fullJid) {
    var sess = this.getMediaSessionToJid(fullJid);
    if (!sess)
        return null;
    var m = sess.remoteMutedState;
    return {
        audio: (sess.remoteStream.getAudioTracks().length > 0) && !m.audioMuted,
        video: (sess.remoteStream.getVideoTracks().length > 0) && !m.videoMuted
    }
 },

 getMediaSessionToJid: function(fullJid) {
 //TODO: We get only the first media session to fullJid, but there may be more
    var sessions = this.jingle.sessions;
    var sess = null;
    for (var sid in sessions) {
        var s = sessions[sid];
        if (s.localStream && (s.peerjid === fullJid)) {
            sess = s;
            break;
        }
    }
    return sess;
 },

/**
  Updates the ICE servers that will be used in the next call.
  @param {array} iceServers An array of ice server objects - same as the iceServers parameter in
          the RtcSession constructor
*/
 updateIceServers: function(iceServers) {
     if (!iceServers || (iceServers.length < 1))
             console.warn("updateIceServers: Null or empty array of ice servers provided");
     this.jingle.iceServers = iceServers;
 },

 /**
    This is a <b>class</b> method (i.e. not called on an instance but directly on RtcSession).
    Registers a callback function that will be called
    repeatedly every 400 ms with the current mic volume level from 0 to 100%, once
    the local media stream is accessible. This can be called multiple times to change
    the callback, but only one callback is registered at any moment,
    for performance reasons.
    @static
    @param {VolumeCb} cb
        The callback function

 */
 volMonAttachCallback: function(cb)
 {
    RtcSession.gVolMonCallback = cb;
 },

 /**
    The volume level callback function
    @callback VolumeCb
    @param {int} level - The volume level in percent, from 0 to 100
 */
 _attachRemoteStreamHandlers: function(stream)
 {
    var at = stream.getAudioTracks();
    for (var i=0; i<at.length; i++)
        at[i].onmute =
        function(e) {
            this.trigger('remote-audio-muted', stream);
        };
    var vt = stream.getVideoTracks();
    for (var i=0; i<vt.length; i++)
        vt[i].muted = function(e) {
            this.trigger('remote-video-muted', stream);
        };
 },
 _refLocalStream: function(sendsVideo) {
    RtcSession.gLocalStreamRefcount++;
    if (sendsVideo)
        RtcSession._enableLocalVid(this);
 },
 _unrefLocalStream: function() {
    var cnt = --RtcSession.gLocalStreamRefcount;
    if (cnt > 0)
        return;

    if (!RtcSession.gLocalStream) {
        console.warn('RtcSession.unrefLocalStream: gLocalStream is null. refcount = ', cnt);
        return;
    }
    this._freeLocalStream();
 },
 _freeLocalStream: function() {
    RtcSession.gLocalStreamRefcount = 0;
    if (!RtcSession.gLocalStream)
        return;
    RtcSession._disableLocalVid(this);
/**
    Local stream is about to be closed and local video player to be destroyed
    @event local-video-destroy
    @type {object}
    @property {DOM} player The local video player, which is about to be destroyed
*/
    this.trigger('local-player-remove', {player: RtcSession.gLocalVid});
    RtcSession.gLocalVid = null;
    RtcSession.gLocalStream.stop();
    RtcSession.gLocalStream = null;
 },

 trigger: function(name, obj) {
    if (this.logEvent)
        this.logEvent(name, obj);
    try {
        $(this).trigger(name, [obj]);
    } catch(e) {
        console.warn("Exception thrown from user event handler '"+name+"':\n"+e.stack?e.stack:e);
    }
 },
 /**
    Releases any global resources referenced by this instance, such as the reference
    to the local stream and video. This should be called especially if multiple instances
    of RtcSession are used in a single JS context
 */
 destroy: function() {
    this.hangup();
    this._freeLocalStream();
 },

/** Returns whether the call or file transfer with the given
    sessionId is being relaid via a TURN server or not.
      @param {string} sid The session id of the call
      @returns {integer} 1 if the call/transfer is being relayed, 0 if not, 'undefined' if the
        status is unknown (not established yet or browser does not provide stats interface)
*/
 isRelay: function(sid) {
     var sess = this.jingle.sessions[sid];
     if (!sess || ! sess.statsRecorder)
         return undefined;
     return sess.statsRecorder.isRelay();
 },

 _requiredLocalStream: function(channels) {
    if (channels.video)
        return RtcSession.gLocalAudioVideoStream;
      else
        return RtcSession.gLocalAudioOnlyStream;
  }
}


RtcSession._maybeCreateVolMon = function() {
    if (RtcSession.gVolMon)
        return true;
    if (!RtcSession.gVolMonCallback || (typeof hark !== "function"))
        return false;

    RtcSession.gVolMon = hark(RtcSession.gLocalStream, { interval: 400 });
    RtcSession.gVolMon.on('volume_change',
         function (volume, treshold)
         {
         //console.log('volume', volume, treshold);
            var level;
            if (volume > -35)
                level = 100;
             else if (volume > -60)
                level = (volume + 100) * 100 / 25 - 160;
            else
                level = 0;
            RtcSession.gVolMonCallback(level);
        });
    return true;
}

RtcSession.avFlagsToMutedState =  function(flags, stream) {
    if (!stream)
        return {audio:true, video:true};
    var mutedState = new MutedState;
    var muteAudio = (!flags.audio && (stream.getAudioTracks().length > 0));
    var muteVideo = (!flags.video && (stream.getVideoTracks().length > 0));
    mutedState.set(muteAudio, muteVideo);
    return mutedState;
}

RtcSession.xmlUnescape = function(text) {
    return text.replace(/\&amp;/g, '&')
               .replace(/\&lt;/g, '<')
               .replace(/\&gt;/g, '>')
               .replace(/\&apos;/g, "'")
               .replace(/\&quot;/g, '"');
}

RtcSession._disableLocalVid = function(rtc) {
    if (!this._localVidEnabled)
        return;
// All references to local video are muted, disable local video display
// We need sess only to have where an object to trigger the event on
    RTC.attachMediaStream($(this.gLocalVid), null);
/**
    Local camera playback has been disabled because all calls have muted their video
    @event local-video-disabled
    @type {object}
    @property {DOM} player - the local camera video HTML element
*/
    this._localVidEnabled = false;
    rtc.trigger('local-video-disabled', {player: this.gLocalVid});

}

RtcSession._enableLocalVid = function(rtc) {
    if(this._localVidEnabled)
        return;
    RTC.attachMediaStream($(this.gLocalVid), this.gLocalStream);
/**
    Local video playback has been re-enabled because at least one call started sending video
    @event local-video-enabled
    @type {object}
    @property {DOM} player The local video player HTML element
*/
    rtc.trigger('local-video-enabled', {player: this.gLocalVid});
    this.gLocalVid.play();
    this._localVidEnabled = true;
}

/**
 Creates a unique string identifying the call,
 that is independent of whether the
 caller or callee generates it. Used only for sending stats
*/
RtcSession.prototype._makeCallId = function(sess) {
    if (sess.isInitiator)
        return this.ownAnonId+':'+sess.peerAnonId+':'+sess.sid;
      else
        return sess.peerAnonId+':'+this.ownAnonId+':'+sess.sid;
}
/**
 Anonymizes a JID
*/
RtcSession._anonJid = function(jid) {
    return MD5.hexdigest(Strophe.getBareJidFromJid(jid)+"webrtc stats collection");
}

/**
    Session object
    This is an internal object, but the following properties are useful for the library user.
    @constructor
*/
function SessWrapper(sess) {
    this._sess = sess;
}

SessWrapper.prototype = {

/**
    The remote peer's full JID
    @returns {string}
*/
peerJid: function(){
    return this._sess.peerjid;
},

/**
    Our own JID
    @returns {string}
*/
jid:function() {
return this._sess.jid;
},

/**
  The stream object of the stream received from the peer
    @returns {MediaStream}
*/
remoteStream: function() {
    return this._sess.remoteStream;
},

/**
    The Jingle session ID of this session
    @returns {string}
*/
sid: function() {
    return this._sess.sid;
},

/**
    True if we are the caller, false if we answered the call
    @returns {boolean}
*/
isCaller: function() {
    return this._sess.isInitiator;
},

/**
    True if this is a dummy session object and there was no established session before
    the call ended, or the session was closed before emitting the event.
    The object's peerJid() and isCaller() methods are guaranteed to return
    a meaningful value, sid() may or may not return a session id. The other
    getters will return <i>undefined</i>.
    This type of dummy session is passed only to the call-ended
    event handler, and this happens when an error occurred. The reason and text
    event properties carry more info about the error
*/
isFake: function() {
    return (this._sess.isFake === true);
},

/** Returns whether a call or file transfer is being relayed through a TURN server or not.
      @returns {integer} If the status in unknown (not established yet or no stats
         provided by browser, e.g. Firefox), the return value is 'undefined', otherwise
         it is 0 if call is direct and 1 if call is relayed
*/
isRelay: function() {
    var statsRec = this._sess.statsRecorder;
    if (!statsRec)
        return undefined;
    else
        return statsRec.isRelay();
}
}
function getStreamAv(stream) {
    if (!stream)
        return {audio:false, video: false};

    var result = {};
    result.audio = (stream.getAudioTracks().length > 0);
    result.video = (stream.getVideoTracks().length > 0);
    return result;
}

RtcSession.xorEnc = function(str, key) {
  var int2hex = ['0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'];

  var result = "";
  var j = 0;
  var len = str.length;
  var keylen = key.length;
  for (var i = 0; i < len; ++i) {
      var code = str.charCodeAt(i) ^ key.charCodeAt(j++);
      if (j >= keylen)
          j = 0;
      result+=int2hex[code>>4];
      result+=int2hex[code&0x0f];
  }
  return result;
}

RtcSession.xorDec = function(str, key) {
    var result = "";
    var len = str.length;
    var j = 0;
    if (len & 1)
        throw new Error("Not a proper hex string");
    var keylen = key.length;
    for (var i=0; i<len; i+=2) {
        var code = (RtcSession.hexDigitToInt(str.charAt(i)) << 4)|
            RtcSession.hexDigitToInt(str.charAt(i+1));
        code ^= key.charCodeAt(j++);
        if (j >= keylen)
            j = 0;
        result+=String.fromCharCode(code);
    }
    return result;
}

RtcSession.hexDigitToInt = function(digit) {
    var code = digit.charCodeAt(0);
    if (code > 47 && code < 58)
        return code-48;
    else if (code > 96 && code < 103)
        return code-97+10;
    else if (code > 64 && code < 71)
        return code-65+10;
    else
        throw new Error("Non-hex char");
};

