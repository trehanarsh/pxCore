#ifndef __RT_RPC_STREAM_H__
#define __RT_RPC_STREAM_H__

#include "rtRemoteTypes.h"
#include "rtSocketUtils.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

class rtRemoteStreamSelector;

class rtRemoteStream : public std::enable_shared_from_this<rtRemoteStream>
{
  friend class rtRemoteStreamSelector;

public:
  enum class State
  {
    Opened,
    Closed
  };

  using MessageHandler = rtError (*)(rtJsonDocPtr const& doc, void* argp);
  using StateChangedHandler = rtError (*)(std::shared_ptr<rtRemoteStream> const& stream,
    State state, void* argp);

  rtRemoteStream(rtRemoteEnvironment* env, int fd,
    sockaddr_storage const& local_endpoint, sockaddr_storage const& remote_endpoint);
  ~rtRemoteStream();

  rtRemoteStream(rtRemoteStream const&) = delete;
  rtRemoteStream& operator = (rtRemoteStream const&) = delete;

  inline bool isConnected() const
    { return m_fd != -1; }

  rtError open();
  rtError close();
  rtError connect();
  rtError connectTo(sockaddr_storage const& endpoint);
  rtError send(rtJsonDocPtr const& msg);
  rtRemoteAsyncHandle sendWithWait(rtJsonDocPtr const& msg);
  rtError setMessageHandler(MessageHandler handler, void* argp);
  rtError setStateChangedHandler(StateChangedHandler handler, void* argp);

  inline sockaddr_storage getLocalEndpoint() const
    { return m_local_endpoint; }

  inline sockaddr_storage getRemoteEndpoint() const
    { return m_remote_endpoint; }

private:
  rtError onIncomingMessage(rtSocketBuffer& buff, time_t now);

private:
  int                           m_fd;
  time_t                        m_last_message_time;
  time_t                        m_last_ka_message_time;
  Callback<MessageHandler>      m_message_handler;
  Callback<StateChangedHandler> m_state_changed_handler;
  sockaddr_storage              m_local_endpoint;
  sockaddr_storage              m_remote_endpoint;
  rtRemoteEnvironment*          m_env;
};

class rtRemoteStreamSelector
{
public:
  rtRemoteStreamSelector();

  rtError start();
  rtError registerStream(std::shared_ptr<rtRemoteStream> const& s);
  rtError removeStream(std::shared_ptr<rtRemoteStream> const& s);
  rtError shutdown();

private:
  static void* pollFds(void* argp);
  rtError doPollFds();

private:
  std::vector< std::shared_ptr<rtRemoteStream> >  m_streams;
  pthread_t                                       m_thread;
  std::mutex                                      m_mutex;
  int                                             m_shutdown_pipe[2];
};


#endif
