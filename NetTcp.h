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

// 目前通用的tcp udp v4地址
typedef struct sockaddr_in sockaddr_t;  // 与sockaddr 有区别

namespace GAG
{
	class NetTcp
	{
	public:
		enum class Status : int32_t
		{
			ConnectIng	= 0,
			ConnectOK	= -1,
			ConnectFail = -2,
			NetError	= -3,
			Timeout		= -4,
			CloseByPeer = -5,
		};

		NetTcp() {}
		explicit NetTcp(kj::String&& name, std::string& addr);
		~NetTcp();
		NetTcp& operator=(NetTcp&);

		const kj::String& GetName() const { return name; }
		bool Init(int id, int64_t& now);

		void SendMsg(int id, bool response, int session, int code, kj::Array<const capnp::word>&& data);
		bool ReceiveMsg(int id, int64_t& now);

		bool CheckTimeout(int id, int64_t& now);
		void SendPing(int id, int64_t& now);

	private:

		kj::String	name;
		std::string addr;

		SOCKET s_;
		std::string send_buffer;
		std::string recv_buffer;
		Status status;

		int64_t client_timestamp; // ping when client send
		int64_t server_timestamp; // ping when client recv from server
		int64_t last_recv_timestamp;

	private:
		void SocketStart();
		int  SocketSetNonblock();
		int  SocketClose();
		int  SocketConnect(const sockaddr_t * addr, int id, int64_t& now);
		int	 SocketSelectConnect(int ms);

		bool CheckMessageComplete(int id);
		void DispatchMessage(int id, int64_t& now);
	};
}
