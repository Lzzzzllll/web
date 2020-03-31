#include "../utils/PCH.h"
#include "NetHost.h"
#include "NetTcp.h"
#include "NetFilter.h"
#include "../utils/kjlua.h"
#include <chrono>


#define LOG_MOD "NetHost"

namespace GAG
{
	kj::AsyncIoContext& ThreadIo();

	NetHost* NetHost::instance = nullptr;

	void NetHost::Init()
	{
		Quit();
		instance = new NetHost();

	}

	void NetHost::Quit()
	{
		delete instance;
		instance = nullptr;
	}

	void NetHost::ThreadRun()
	{
		instance->Run();
	}

	void NetHost::ThreadStop()
	{
		instance->Stop();
	}

	NetControl* NetHost::Control()
	{
		if (instance)
		{
			return &instance->netControl;
		}
		else
		{
			return nullptr;
		}
	}

	NetHost::NetHost()
	{

	}

	NetHost::~NetHost()
	{
	}

	void NetHost::Run()
	{
		auto& io = ThreadIo();

		std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
		int64_t now = (int64_t)tp.time_since_epoch().count();

		for (;;)
		{
			for (auto& pair : connections)
			{
				auto id = pair.first;
				auto* c = pair.second;
				if (c->GetName() != "login")
				{
					c->SendPing(id, now);
				}
			}

			KJ_IF_MAYBE(req, netControl.queueReq.Dequeue())
			{
				KJ_SWITCH_ONEOF((*req))
				{
					KJ_CASE_ONEOF(msg, NetControl::Open)
					{
						GAG::NetTcp* c = new GAG::NetTcp(kj::mv(msg.name), msg.addr);
						connections[msg.id] = c;
						if (!c->Init(msg.id, now))
						{
							LogWarn("init err", msg.id, c->GetName().cStr());
							auto con_ptr = FindConnection(msg.id);
							SAFE_DELETE(con_ptr);
							connections.erase(msg.id);
							int status = (int)NetTcp::Status::ConnectFail;
							NetHost::Control()->queueRep.Enqueue(NetControl::Recv{ msg.id, false, status, status, nullptr });
						}
					}
					KJ_CASE_ONEOF(msg, NetControl::Send)
					{
						LogDebug("queue send", msg.id, msg.side, msg.session, msg.code);
						if (auto* c = FindConnection(msg.id))
						{
							c->SendMsg(msg.id, msg.side, msg.session, msg.code, kj::mv(msg.data));
							LogDebug("queue send ok", msg.id, msg.side, msg.session, msg.code);
						}
					}
					KJ_CASE_ONEOF(msg, NetControl::Close)
					{
						LogWarnFmt("lua_close id:%d now:%lld", msg.id, now/1000);
						auto con_ptr = FindConnection(msg.id);
						SAFE_DELETE(con_ptr);
						connections.erase(msg.id);
					}
					KJ_CASE_ONEOF(msg, NetControl::Filter)
					{
						auto name = kj::str(msg.name);
						auto it = filters.find(name);
						if (it == filters.end())
						{
							if (msg.addFilter)
							{
								LogDebug("Filter Add", name, msg.addFilter, msg.delFilter);
								filters.emplace(kj::str(name), msg.addFilter);
							}
						}
						else
						{
							if (msg.addFilter != it->second)
							{
								msg.delFilter = it->second;
								if (msg.addFilter)
								{
									LogDebug("Filter Replace", name, msg.addFilter, msg.delFilter);
									msg.delFilter->FilterMessage(NetControl::Filter{ kj::str(name), msg.delFilter, msg.addFilter });
									it->second = msg.addFilter;
								}
								else
								{
									LogDebug("Filter Del", name, msg.addFilter, msg.delFilter);
									msg.delFilter->FilterMessage(NetControl::Filter{ kj::str(name), msg.delFilter, msg.addFilter });
									filters.erase(it);
								}
							}
						}
						Rep(name, kj::mv(msg));
					}
					// OnAppPause
				}
			}
			else
			{
				for (auto& pair : connections)
				{
					auto id = pair.first;
					auto* c = pair.second;

					c->CheckTimeout(id, now);
					c->ReceiveMsg(id, now);
				}
				io.provider->getTimer().afterDelay(3 * kj::MILLISECONDS).wait(io.waitScope);
				return;
			}
		}
	}

	void NetHost::Stop()
	{
		for (auto& pair : connections)
		{
			SAFE_DELETE(pair.second);
		}
		connections.clear();
	}

	void NetHost::Rep(const kj::String& name, NetControl::Rep&& rep)
	{
		auto it = filters.find(name);
		if (it == filters.end() || !it->second->FilterMessage(kj::mv(rep)))
		{
			netControl.queueRep.Enqueue(kj::mv(rep));
		}
	}

	NetTcp* NetHost::FindConnection(int id)
	{
		auto it = connections.find(id);
		if (it == connections.end())
			return nullptr;
		return it->second;
	}
}

