#include "../utils/PCH.h"

#include "NetTcp.h"
#include "NetControl.h"
#include "NetHost.h"

#define IGNORE_SIGNAL(sig)    signal(sig, SIG_IGN)
#define LOG_MOD "NetTcp"
#define RECV_PING_INTERVAL 4000
#define SEND_PING_INTERVAL 3
#define CONN_INTERVAL	   3000

#ifdef _MSC_VER
#undef	errno
#define errno				WSAGetLastError()
#define IGNORE_SIGPIPE()
#define ECONNECTED			WSAEWOULDBLOCK  //WSAEINPROGRESS
#define ESEND_1				WSAEWOULDBLOCK
#define ESEND_2				WSAEWOULDBLOCK //WSAETIMEDOUT

#define NET_EINTR			WSAEINTR
#define NET_EINPROGRESS     WSAEWOULDBLOCK  //WSAEINPROGRESS
#endif

#ifdef __ANDROID__
#define INVALID_SOCKET      (~0)
#define SOCKET_ERROR        (-1)
#define IGNORE_SIGPIPE()    IGNORE_SIGNAL(SIGPIPE)

#define ESEND_1				EWOULDBLOCK
#define ESEND_2				EAGAIN

#define NET_EINTR			EINTR
#define NET_EINPROGRESS     EINPROGRESS
typedef int socket_t;
#endif

namespace LZ
{
	NetTcp::NetTcp(kj::String&& name, std::string& _addr) : name(kj::mv(name)), addr(_addr), status(Status::ConnectOK), client_timestamp(0), server_timestamp(0), last_recv_timestamp(0)
	{
	}

	NetTcp::~NetTcp()
	{
		LogWarn("~NetTcp");
		socket_close();
	}

	NetTcp& NetTcp::operator=(NetTcp& o)
	{
		s_ = o.s_;
		return *this;
	}

	int NetTcp::socket_set_nonblock() 
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

	void NetTcp::socket_start()
	{
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
		WSADATA wsad;
		WSAStartup(WINSOCK_VERSION, &wsad);
		atexit(_socket_start);
#endif
		IGNORE_SIGPIPE();
	}

	int NetTcp::socket_close(Status _status)
	{
		status = _status;

#ifdef _MSC_VER
		return closesocket(s_);
#endif

#ifdef __ANDROID__
		int ret = close(s_);
		if (ret == INVALID_SOCKET)
		{
			int err = errno;
			LogWarn("socket_close error ", err);
			return err;
		}
		LogWarn("socket_close ", s_, (int)_status, name.cStr());
		return ret;
#endif
	}

	bool NetTcp::Init(int id, int64_t& now)
	{
		socket_start();

		s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_ == INVALID_SOCKET)
		{
			int err = errno;
			LogWarn("socket_create error r", INVALID_SOCKET, err, name.cStr(), (int)s_);
			return false;
		}
		LogWarn("socket_create", (int)status, name.cStr(), (int)s_);

		if (socket_set_nonblock() < 0)
		{
			LogWarn("socket_set_noblock error");
			return false;
		}

		size_t position = addr.find(':');
		std::string Ip(addr, 0, position);
		std::string Port(addr, position + 1, addr.size());

		hostent *he;
		if ((he = gethostbyname(Ip.c_str())) == 0)
		{
			LogWarn("socket_gethostbyname error", addr.c_str());
			return false;
		}

		sockaddr_in saddr;
		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(atoi(Port.c_str()));
		saddr.sin_addr = *((in_addr *)he->h_addr);
		memset(&(saddr.sin_zero), 0, 8);

		int ret = socket_connect(&saddr);
		if (ret < 0)
		{
			LogWarn("socket_connect error", ret, (int)s_);
			return false;
		}

		if (name != "login")
		{
			client_timestamp = now;
			NetHost::Control()->queueReq.Enqueue(NetControl::Send{ id, 0, 0, 0xFFFF, nullptr });
			LogWarnFmt("send_ping_debug %lld", now / 1000);
		}

		return true;
	}

	int NetTcp::socket_connect(const sockaddr_t * addr)
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
					LogWarn("socket_connect", (int)s_, ret, err);
					continue;
				}
				else if (err != NET_EINPROGRESS)
				{
					return -2;
				}
				else
				{
					break;
				}
			}
		}
		status = NetTcp::Status::ConnectIng;
		return 0;
	}

	int NetTcp::socket_select_connect(int ms)
	{
		fd_set wset;
		FD_ZERO(&wset);
		FD_SET(s_, &wset);
		struct timeval tv = { ms / 1000 , 0};

		int ret = select((int)s_ + 1, nullptr, &wset, nullptr, &tv);
		if (ret <= 0)
		{
			int err = errno;
			LogWarn("socket_select errno", (int)s_, ret, err);
			return ret;
		}

		if (FD_ISSET(s_, &wset))
		{
			int error;
			socklen_t error_len = sizeof(error);
			ret = getsockopt(s_, SOL_SOCKET, SO_ERROR, (char*)&error, &error_len);
			if (ret == -1 || error != 0)
			{
				return -3;
			}
		}
		return 0;
	}

	void NetTcp::SendMsg(int id, bool response, int session, int code, kj::Array<const capnp::word>&& data)
	{
		if (s_ == INVALID_SOCKET || status != Status::ConnectOK)
		{
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
			if (err != ESEND_1 && err != ESEND_2)
			{
				LogWarn("socket_send_error errno", err);
				socket_close(Status::NetError); // repeat close err?
				NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
			}
		}
		else
		{
			send_buffer.erase(0, sendlen);
			//LogDebug("socket_send_part, reserve", sendlen, send_buffer.size());
		}
	}

	bool NetTcp::ReceiveMsg(int id, int64_t& now)
	{
		if (s_ == INVALID_SOCKET || status != Status::ConnectOK)
		{
			return false;
		}

		if (IsMessageComplete(id))
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
			LogWarn("socket_recv_timeout 0 id", id);
			socket_close(Status::CloseByPeer);
			NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
			return true;
		}
		else if (rv < 0)
		{
			int error = errno;
			if (error != ESEND_1 && error != ESEND_2)
			{
				LogWarn("socket_recv_error ", rv, error);
				socket_close(Status::NetError);
				NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, false, (int)status, (int)status, nullptr });
			}
			return false;
		}

		t.assign(buf, rv);
		recv_buffer += t;
		//LogDebug("recv_per", id, t.size(), recv_buffer.size());

		if (!IsMessageComplete(id))
		{
			return false;
		}

		DispatchMessage(id, now);
		return true;
	}

	bool NetTcp::IsMessageComplete(int id)
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

		if (packsize > recv_buffer.size())
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

		//LogDebug("recv_suc", id, header->size, size, recv_buffer.size());
		//LogDebug("recv_suc", id, side, session, size, code, size);

		if (size > 0)
		{
			// session = 0, code = 0xFFFF, data_size = sizeof(int64_t)
			if (session == 0 && code == 0xFFFF) // if ping 
			{
				
				int64_t* timestamp_arr = (int64_t*)(recv_buffer.c_str() + sizeof(NetHeader));

				data = kj::heapArray<capnp::word>(size + sizeof(double) / sizeof(capnp::word));
				double* data_arr = (double*)data.begin();
				data_arr[0] = (timestamp_arr[0] - (now + client_timestamp) / 2.0) / 1000.0;
				data_arr[1] = (now - client_timestamp) / 1000.0;
				server_timestamp = now;
				//LogDebug("recv_ping", timestamp_arr[0], data_arr[1]);
			}
			else
			{
				data = kj::heapArray<capnp::word>(size);
				memcpy(data.begin(), recv_buffer.c_str() + sizeof(NetHeader), size * sizeof(capnp::word));
			}
		}
		last_recv_timestamp = now;

		int dec = size * sizeof(capnp::word) + sizeof(NetHeader);
		recv_buffer.erase(0, dec);
		//LogDebug("recv_clear_buffer", id, recv_buffer.size(), dec);
		//maybe filter
		NetControl::Rep rep = NetControl::Recv{ id, side, session, code, data.size() ? kj::mv(data) : nullptr };
		NetHost::instance->Rep(GetName(), kj::mv(rep));
	}

	bool NetTcp::CheckTimeout(int id)
	{
		if (last_recv_timestamp == 0 || client_timestamp == 0 || server_timestamp == 0)
		{
			return false;
		}

		if (GetName() == "login")
		{
			return false;
		}

		if (status == Status::Timeout)
		{
			return false;
		}

		if (last_recv_timestamp - server_timestamp < RECV_PING_INTERVAL && client_timestamp - server_timestamp < RECV_PING_INTERVAL)
		{
			return false;
		}

		status = NetTcp::Status::Timeout;
		NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, 0, (int)status, (int)status, nullptr });
		LogWarnFmt("id:%d CheckTimeout last_recv_timestamp:%lld, client_timestamp:%lld, server_timestamp:%lld", id, last_recv_timestamp/1000, client_timestamp/1000, server_timestamp/1000);

		return true;
	}

	void NetTcp::SendPing(int id, int64_t& now)
	{
		int64_t interval = (now - client_timestamp) / 1000;
		if (interval > 0 && interval % SEND_PING_INTERVAL == 0)
		{
			client_timestamp = now;
			NetHost::Control()->queueReq.Enqueue(NetControl::Send{ id, 0, 0, 0xFFFF, nullptr });
			LogWarnFmt("send_ping_debug id:%d interval:%lld now:%lld %lld %s", id, interval, now/1000, client_timestamp/1000, name.cStr());
		}
	}

	bool NetTcp::CheckConnectIng(int id, int64_t& now)
	{
		if (status == NetTcp::Status::ConnectIng)
		{
			int ret = socket_select_connect(CONN_INTERVAL);
			if (ret < 0)
			{
				status = NetTcp::Status::ConnectFail;
			}
			else
			{
				status = NetTcp::Status::ConnectOK;
				if (name != "login")
				{
					client_timestamp = now;
					NetHost::Control()->queueReq.Enqueue(NetControl::Send{ id, 0, 0, 0xFFFF, nullptr });
					LogWarnFmt("send_ping_debug %lld", now / 1000);
				}
			}
			NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ id, 0, (int)status, (int)status, nullptr });
			return true;
		}
		return false;
	}
}
