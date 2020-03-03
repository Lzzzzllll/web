#include "../utils/PCH.h"
#include <queue>
#include "../utils/kjlua.h"

#include <luacapnp/luamodule.h>
lua_CFunction reexport_luaopen_luacapnp = luaopen_luacapnp;

#include "NetHost.h"

#if LUA_VERSION_NUM<502
#define lua_rawlen lua_objlen
#endif

#define LOG_MOD "LuaRpc"

namespace GAG
{
	static int id = 0;
	//[-1, +1, m] name, addr -> connection
	static int lopen(lua_State *L)
	{
		const char* name = luaL_checkstring(L, 1);
		const char* addr = luaL_checkstring(L, 2);
		id++;
		NetHost::Control()->queueReq.Enqueue(NetControl::Open{ id, kj::str(name), addr });
		lua_pushinteger(L, id);
		return 1;
	}

	//[-4|5, +0, m] connection, side, session, code, data
	static int lsend(lua_State *L)
	{
		int c = (int)lua_tointeger(L, 1);
		bool lside = lua_toboolean(L, 2) != 0;
		int lsession = (int)lua_tointeger(L, 3);
		int lcode = (int)lua_tointeger(L, 4);
		size_t size = 0;
		const char *data = nullptr;
		if (lua_gettop(L) >= 5)
		{
			data = luaL_checklstring(L, 5, &size);
		}
		auto buffer = kj::heapArray<capnp::word>((size + sizeof(capnp::word) - 1) / sizeof(capnp::word));
		if (size > 0)
		{
			memcpy(buffer.begin(), data, size);
		}
		NetHost::Control()->queueReq.Enqueue(NetControl::Send{ c, lside, lsession, lcode, kj::mv(buffer) });
		LogWarn("lua send", c, lside, lsession, lcode);  // TODO
		return 0;
	}

	static std::map<int, std::queue<NetControl::Recv> > recvQueues;
	//[-1, +0|5, m] connection -> side, session, code, data, data2
	static int lrecv(lua_State *L)
	{
		int c = (int)lua_tointeger(L, 1);
		auto it = recvQueues.find(c);
		if (it != recvQueues.end())
		{
			auto& q = it->second;
			if (!q.empty())
			{
				auto& msg = q.front();
				lua_pushboolean(L, msg.side);
				lua_pushinteger(L, msg.session);
				lua_pushinteger(L, msg.code);
				if (msg.session == 0 && msg.code == 0xFFFF)
				{
					assert(msg.data.size() == 2 * sizeof(double));
					double* data_arr = (double*)msg.data.begin();
					lua_pushnumber(L, data_arr[0]);
					lua_pushnumber(L, data_arr[1]);
					LogDebug("recv ping pop", c, msg.id, msg.side, msg.session, msg.code, data_arr[0], data_arr[1]);
					q.pop();
					return 5;
				}
				else
				{
					lua_pushlstring(L, (const char*)msg.data.begin(), msg.data.size() * sizeof(msg.data[0]));
				}

				LogWarn("recv pop", c, msg.id, msg.side, msg.session, msg.code);  // TODO
				q.pop();
				return 4;
			}
		}
		for (;;)
		{
			KJ_IF_MAYBE(rep, NetHost::Control()->queueRep.Dequeue())
			{
				KJ_SWITCH_ONEOF((*rep))
				{
					KJ_CASE_ONEOF(msg, NetControl::Recv)
					{
						if (msg.id == c)
						{
							lua_pushboolean(L, msg.side);
							lua_pushinteger(L, msg.session);
							lua_pushinteger(L, msg.code);
							if (msg.session == 0 && msg.code == 0xFFFF)
							{
								assert(msg.data.size() == 2 * sizeof(double));
								double* data_arr = (double*)msg.data.begin();
								lua_pushnumber(L, data_arr[0]);
								lua_pushnumber(L, data_arr[1]);
								LogDebug("recv ping good", c, msg.id, msg.side, msg.session, msg.code, data_arr[0], data_arr[1]);
								return 5;
							}
							else
							{
								lua_pushlstring(L, (const char*)msg.data.begin(), msg.data.size() * sizeof(msg.data[0]));
							}
							LogWarn("recv good", c, msg.id, msg.side, msg.session, msg.code); // TODO
							return 4;
						}
						else
						{
							auto& q = recvQueues[msg.id];
							LogWarn("recv push", c, msg.id, msg.side, msg.session, msg.code);  // TODO
							q.push(kj::mv(msg));
						}
					}
				}
			}
		else
		{
			return 0;
		}
		}
	}

	//[-1, +0, m] connection
	static int lclose(lua_State *L)
	{
		int c = (int)lua_tointeger(L, 1);
		NetHost::Control()->queueReq.Enqueue(NetControl::Close{ c });
		return 1;
	}

	static const luaL_Reg luanprotolib[] = {
		{ "open", lopen },
		{ "send", lsend },
		{ "recv", lrecv },
		{ "close", lclose },
		{ NULL, NULL }
	};

	extern "C" LIBBATTLE_API int
		luaopen_luarpc(lua_State *L)
	{
		luaL_newlib(L, luanprotolib);
		return 1;
	}
}
