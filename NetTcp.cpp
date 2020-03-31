#include "../utils/PCH.h"

#include "NetTcp.h"
#include "NetControl.h"
#include "NetHost.h"

#define IGNORE_SIGNAL(sig)				signal(sig, SIG_IGN)
#define LOG_MOD							"NetTcp"
#define RECV_PING_INTERVAL				4000
#define SEND_PING_INTERVAL				3
#define CONN_INTERVAL					10000
#define MAX_PROTO_SIZE					50 * 1024 * 1024 

#define NET_CONTROL_RECV(session, code)  NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, 0, session, code, nullptr })
#define NET_CONTROL_SEND(session, code)  NetHost::Control()->queueReq.Enqueue(NetControl::Send{ id, 0, session, code, nullptr })

#ifdef _MSC_VER
#undef	errno
#define errno				WSAGetLastError()
#define IGNORE_SIGPIPE()

#define NET_EWOULDBLOCK		WSAEWOULDBLOCK
#define NET_EAGAIN			WSAEWOULDBLOCK

#define NET_EINTR			WSAEINTR
#define NET_EINPROGRESS     WSAEWOULDBLOCK
#endif

#ifdef __ANDROID__
#define INVALID_SOCKET      (~0)
#define SOCKET_ERROR        (-1)
#define IGNORE_SIGPIPE()    IGNORE_SIGNAL(SIGPIPE)

#define NET_EWOULDBLOCK		EWOULDBLOCK
#define NET_EAGAIN			EAGAIN

#define NET_EINTR			EINTR
#define NET_EINPROGRESS     EINPROGRESS
typedef int socket_t;
#endif

namespace GAG
{
	NetTcp::NetTcp(kj::String&& name, std::string& _addr) : name(kj::mv(name)), addr(_addr), status(Status::ConnectOK), client_timestamp(0), server_timestamp(0), last_recv_timestamp(0)
	{
	}

	NetTcp::~NetTcp()
	{
		SocketClose();
	}

	NetTcp& NetTcp::operator=(NetTcp& o)
	{
		s_ = o.s_;
		return *this;
	}

	int NetTcp::SocketSetNonblock() 
	{
#ifdef _MSC_VER
		u_long mode = 1;
		return ioctlsocket(s_, FIONBIO, &mode);
#endif

#ifdef __ANDROID__
		int mode = fcntl(s_, F_GETFL, 0);
		if (mode == SOCKET_ERROR)
			return SOCKET_ERROR;
		if (mode & O_NONBLOCK)
			return 0;
		return fcntl(s_, F_SETFL, mode | O_NONBLOCK);
#endif    
	}

	void NetTcp::SocketStart()
	{
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
		WSADATA wsad;
		WSAStartup(WINSOCK_VERSION, &wsad);
		atexit(_socket_start);
#endif
		IGNORE_SIGPIPE();
	}

	int NetTcp::SocketClose()
	{
#ifdef _MSC_VER
		LogWarn("SocketClose", s_, name.cStr());
		return closesocket(s_);
#endif

#ifdef __ANDROID__
		int ret = close(s_);
		if (ret == INVALID_SOCKET)
		{
			int err = errno;
			LogWarn("SocketClose error ", err);
			return err;
		}
		LogWarn("SocketClose", s_, name.cStr());
		return ret;
#endif
	}

	bool NetTcp::Init(int id, int64_t& now)
	{
		SocketStart();

		s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_ == INVALID_SOCKET)
		{
			int err = errno;
			LogWarn("SocketCreate error", INVALID_SOCKET, s_, err, name.cStr());
			return false;
		}

		if (SocketSetNonblock() < 0)
		{
			LogWarn("SocketSetNonblock error");
			return false;
		}

		size_t position = addr.find(':');
		std::string Ip(addr, 0, position);
		std::string Port(addr, position + 1, addr.size());

		hostent *he;
		if ((he = gethostbyname(Ip.c_str())) == 0)
		{
			LogWarn("Socket gethostbyname error", addr.c_str());
			return false;
		}

		sockaddr_in saddr;
		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(atoi(Port.c_str()));
		saddr.sin_addr = *((in_addr *)he->h_addr);
		memset(&(saddr.sin_zero), 0, 8);

		int ret = SocketConnect(&saddr, id, now);
		if (ret < 0)
		{
			LogWarn("SocketConnect error", ret, s_, name.cStr());
			return false;
		}

		return true;
	}

	int NetTcp::SocketConnect(const sockaddr_t * addr, int id, int64_t& now)
	{
		// for EINTR
		while (1)
		{
			int ret = connect(s_, (const struct sockaddr *)addr, sizeof(*addr));
			if (ret == 0)
			{
				status = NetTcp::Status::ConnectOK;
				return 0;
			}
			else if (ret == -1)
			{
				int err = errno;
				if (err == NET_EINTR)
				{
					LogWarn("SocketConnect", s_, name.cStr(), ret, err);
					continue;
				}
				else if (err != NET_EINPROGRESS)
				{
					LogWarn("SocketConnect", s_, name.cStr(), ret, err);
					return -2;
				}
				else
				{
					break;
				}
			}
		}
		
		int ret = SocketSelectConnect(CONN_INTERVAL);
		if (ret <= 0)
		{
			status = NetTcp::Status::ConnectFail;
			return -3;
		}
		else
		{
			status = NetTcp::Status::ConnectOK;
			if (name != "login")
			{
				client_timestamp = now;
				//NetHost::Control()->queueReq.Enqueue(NetControl::Send{ id, 0, 0, 0xFFFF, nullptr });
				NET_CONTROL_SEND(0, 0xFFFF);
			}
			LogDebug("SocketSelectConnect ok ", s_, name.cStr(), id, ret);
		}
		//NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, 0, (int)status, (int)status, nullptr });
		NET_CONTROL_RECV((int)status, (int)status);
		
		return 0;
	}

	int NetTcp::SocketSelectConnect(int ms)
	{
		fd_set wset;
		FD_ZERO(&wset);
		FD_SET(s_, &wset);
		struct timeval tv = { ms / 1000 , 0};

		int ret = select((int)s_ + 1, nullptr, &wset, nullptr, &tv);
		if (ret <= 0)
		{
			int err = errno;
			LogWarn("SocketSelectConnect err", s_, name.cStr(),ret, err);
			return ret;
		}

		if (FD_ISSET(s_, &wset))
		{
			int error;
			socklen_t error_len = sizeof(error);
			ret = getsockopt(s_, SOL_SOCKET, SO_ERROR, (char*)&error, &error_len);
			if (ret == -1 || error != 0)
			{
				LogWarn("SocketSelectConnect err", s_, name.cStr(), ret, error);
				return -4;
			}
		}
		return 1;
	}

	void NetTcp::SendMsg(int id, bool response, int session, int code, kj::Array<const capnp::word>&& data)
	{
		if (s_ == INVALID_SOCKET || status != Status::ConnectOK)
		{
			LogWarn("SendMsg err", s_, name.cStr(), (int)status);
			return;
		}

		if (response)
		{
			session = session | 0x8000;
		}

		auto size = data.size() * sizeof(data[0]);
		NetHeader h;
		uint32_t hsize = (uint32_t)(size + sizeof(h) - sizeof(h.size));
		h.size = htonl(hsize);
		h.code = code;
		h.session = response ? session | 0x8000 : session & 0x7FFF;

		std::string str_header((char*)&h, sizeof(h));
		send_buffer += str_header;

		size = data.size() * sizeof(data[0]);
		std::string str_body((char*)data.begin(), size);
		send_buffer += str_body;

		int sendlen = send(s_, send_buffer.c_str(), (int)send_buffer.size(), 0);
		if (sendlen <= 0)
		{
			int err = errno;
			if (err != NET_EWOULDBLOCK && err != NET_EAGAIN)
			{
				LogWarn("SendMsg err", sendlen, err);
				status = Status::NetError;
				//NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
				NET_CONTROL_RECV((int)status, (int)status);
			}
		}
		else
		{
			send_buffer.erase(0, sendlen);
		}
	}

	bool NetTcp::ReceiveMsg(int id, int64_t& now)
	{
		if (s_ == INVALID_SOCKET || status != Status::ConnectOK)
		{
			return false;
		}

		if (CheckMessageComplete(id))
		{
			DispatchMessage(id, now);
			return true;
		}

		char buf[1024];
		memset(buf, 0, 1024);
		int rv = recv(s_, buf, 1024, 0);
		std::string t;

		if (rv == 0)
		{
			LogWarn("ReceiveMsg ret=0", id);
			status = Status::CloseByPeer;
			//NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
			NET_CONTROL_RECV((int)status, (int)status);
			return true;
		}
		else if (rv < 0)
		{
			int error = errno;
			if (error != NET_EWOULDBLOCK && error != NET_EAGAIN)
			{
				LogWarn("ReceiveMsg ret=-1 ", rv, error);
				status = Status::NetError;
				//NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
				NET_CONTROL_RECV((int)status, (int)status);
			}
			return false;
		}

		t.assign(buf, rv);
		recv_buffer += t;
		last_recv_timestamp = now;
		//LogDebug("recv_per", id, t.size(), recv_buffer.size());

		if (!CheckMessageComplete(id))
		{
			return false;
		}

		DispatchMessage(id, now);
		return true;
	}

	bool NetTcp::CheckMessageComplete(int id)
	{
		if (recv_buffer.size() < 4)
		{
			return false;
		}

		uint32_t packsize =
			(unsigned char)recv_buffer[0] * (1 << 24) +
			(unsigned char)recv_buffer[1] * (1 << 16) +
			(unsigned char)recv_buffer[2] * (1 << 8) +
			(unsigned char)recv_buffer[3];

		if (packsize > MAX_PROTO_SIZE)
		{
			LogWarn("CheckMessageComplete", id, packsize, recv_buffer.size());
			status = Status::NetError;
			//NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
			NET_CONTROL_RECV((int)status, (int)status);
			return false;
		}

		if (packsize + sizeof(NetHeader::size) > recv_buffer.size())
		{
			//LogDebug("IsMessageComplete", id, packsize, recv_buffer.size());
			return false;
		}

		return true;
	}

	void NetTcp::DispatchMessage(int id, int64_t& now)
	{
		bool side = false;
		int session = 0;
		int code = 0;
		int size = 0;
		kj::Array<capnp::word> data;
		NetHeader* header = (NetHeader*)(recv_buffer.c_str());

		header->size = ntohl(header->size);
		side = (header->session & 0x8000) != 0;
		session = header->session & 0x7FFF;
		code = header->code;
		size = (header->size - sizeof(NetHeader) + sizeof(header->size)) / sizeof(capnp::word);

		if (size > 0)
		{
			// decide on ping, session = 0, code = 0xFFFF; data_size = sizeof(int64_t)
			if (session == 0 && code == 0xFFFF)
			{
				int64_t* timestamp_arr = (int64_t*)(recv_buffer.c_str() + sizeof(NetHeader));
				data = kj::heapArray<capnp::word>(size + sizeof(double) / sizeof(capnp::word));
				double* data_arr = (double*)data.begin();
				data_arr[0] = (timestamp_arr[0] - (now + client_timestamp) / 2.0) / 1000.0;
				data_arr[1] = (now - client_timestamp) / 1000.0;
				server_timestamp = now;
				LogWarn("RecvPing", timestamp_arr[0], data_arr[1], now / 1000);
			}
			else
			{
				data = kj::heapArray<capnp::word>(size);
				memcpy(data.begin(), recv_buffer.c_str() + sizeof(NetHeader), size * sizeof(capnp::word));
			}
		}

		int dec = size * sizeof(capnp::word) + sizeof(NetHeader);
		recv_buffer.erase(0, dec);
		//LogDebug("recv_clear_buffer", id, recv_buffer.size(), dec);

		//maybe filter
		NetControl::Rep rep = NetControl::Recv{ id, side, session, code, data.size() ? kj::mv(data) : nullptr };
		NetHost::instance->Rep(GetName(), kj::mv(rep));
	}

	bool NetTcp::CheckTimeout(int id, int64_t& now)
	{
		if (status == Status::Timeout)
		{
			return false;
		}

		if (now - last_recv_timestamp < RECV_PING_INTERVAL)  // for game, arena, login
		{
			return false;
		}

		if (client_timestamp <= 0 || server_timestamp <= 0 || client_timestamp - server_timestamp < RECV_PING_INTERVAL)  // for login
		{
			return false;
		}

		status = NetTcp::Status::Timeout;
		//NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, 0, (int)status, (int)status, nullptr });
		NET_CONTROL_RECV((int)status, (int)status);
		LogWarn("CheckTimeout", id, last_recv_timestamp/1000, client_timestamp/1000, server_timestamp/1000, now/1000);

		return true;
	}

	void NetTcp::SendPing(int id, int64_t& now)
	{
		if (s_ == INVALID_SOCKET || status != Status::ConnectOK)
		{
			return;
		}

		int64_t interval = (now - client_timestamp) / 1000;
		if (interval > 0 && interval % SEND_PING_INTERVAL == 0)
		{
			client_timestamp = now;
			//NetHost::Control()->queueReq.Enqueue(NetControl::Send{ id, 0, 0, 0xFFFF, nullptr });
			NET_CONTROL_SEND(0, 0xFFFF);
			//LogWarn("SendPing", id, interval, now/1000, (client_timestamp - server_timestamp)/1000, (now - last_recv_timestamp)/1000);
		}
	}
}
