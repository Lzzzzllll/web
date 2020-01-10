#pragma once
#include "NetHeader.h"

#ifdef _MSC_VER
#include <WinSock2.h> //for htonl ntohl
#include <ws2tcpip.h>
#define IGNORE_SIGPIPE()

typedef int socklen_t;
typedef SOCKET socket_t;

static inline void _socket_start(void) { WSACleanup(); }
#endif

#ifdef __ANDROID__
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/select.h>
typedef int SOCKET;
#endif

typedef struct sockaddr_in sockaddr_t;

class NetTcp
{
public:
  enum class Status : int32_t
  {
    ConnectIng = 0,
    ConnectOK = -1,
    ConnectFail = -2,
    NetError = -3,
    Timeout = -4,
    CloseByPeer = -5,
  };

  NetTcp() {}
  explicit NetTcp(kj::String&& name, std::string& addr);
  const kj::String& GetName() const { return name; }

  ~NetTcp();
  NetTcp& operator=(NetTcp&);

  bool Init(int id, int64_t& now);
  void SendMsg(int id, bool response, int session, int code, kj::Array<const capnp::word>&& data);
  bool ReceiveMsg(int id, int64_t& now);

  bool CheckTimeout(int id);
  bool CheckConnectIng(int id, int64_t& now);
  void SendPing(int id, int64_t& now);

  Status GetStatus() { return status; }
  int64_t GetLastRecvTime() { return last_recv_timestamp; }
  int64_t GetClientTime() { return client_timestamp; }
  int64_t GetServerTime() { return server_timestamp; }

  void SetStatus(Status _status) { status = _status; }
  void SetClientTime(int64_t& time) { client_timestamp = time; }

private:

  kj::String	name;
  std::string addr;

  SOCKET s_;
  std::string send_buffer;
  std::string recv_buffer;
  Status status;

  int64_t client_timestamp;
  int64_t server_timestamp;
  int64_t last_recv_timestamp;

  void socket_start(void);
  int  socket_set_nonblock();
  int  socket_close(Status _status = Status::ConnectOK);
  int  socket_connect(const sockaddr_t * addr);
  int	 socket_select_connect(int ms);

  bool IsMessageComplete(int id);
  void DispatchMessage(int id, int64_t& now);
};
