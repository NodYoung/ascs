
#include <iostream>

//configuration
#define ASCS_SERVER_PORT		9527
#define ASCS_REUSE_OBJECT //use objects pool
//#define ASCS_FREE_OBJECT_INTERVAL	60 //it's useless if ASCS_REUSE_OBJECT macro been defined
//#define ASCS_SYNC_DISPATCH //do not open this feature, see below for more details
#define ASCS_DISPATCH_BATCH_MSG
#define ASCS_ENHANCED_STABILITY
//#define ASCS_FULL_STATISTIC //full statistic will slightly impact efficiency
#define ASCS_USE_STEADY_TIMER
#define ASCS_ALIGNED_TIMER
#define ASCS_AVOID_AUTO_STOP_SERVICE
#define ASCS_DECREASE_THREAD_AT_RUNTIME
//#define ASCS_MAX_SEND_BUF	65536
//#define ASCS_MAX_RECV_BUF	65536
//if there's a huge number of links, please reduce messge buffer via ASCS_MAX_SEND_BUF and ASCS_MAX_RECV_BUF macro.
//please think about if we have 512 links, how much memory we can accupy at most with default ASCS_MAX_SEND_BUF and ASCS_MAX_RECV_BUF?
//it's 2 * 1M * 512 = 1G

//use the following macro to control the type of packer and unpacker
#define PACKER_UNPACKER_TYPE	0
//0-default packer and unpacker, head(length) + body
//1-packer2 and unpacker2, head(length) + body
//2-fixed length packer and unpacker
//3-prefix and/or suffix packer and unpacker

#if 1 == PACKER_UNPACKER_TYPE
#if defined(_MSC_VER) && _MSC_VER <= 1800
#define ASCS_DEFAULT_PACKER packer2<shared_buffer<i_buffer>>
#else
#define ASCS_DEFAULT_PACKER packer2<>
#endif
#define ASCS_DEFAULT_UNPACKER unpacker2<>
#elif 2 == PACKER_UNPACKER_TYPE
#undef ASCS_HEARTBEAT_INTERVAL
#define ASCS_HEARTBEAT_INTERVAL	0 //not support heartbeat
#define ASCS_DEFAULT_PACKER fixed_length_packer
#define ASCS_DEFAULT_UNPACKER fixed_length_unpacker
#elif 3 == PACKER_UNPACKER_TYPE
#define ASCS_DEFAULT_PACKER prefix_suffix_packer
#define ASCS_DEFAULT_UNPACKER prefix_suffix_unpacker
#endif
//configuration

#include <ascs/ext/tcp.h>
using namespace ascs;
using namespace ascs::tcp;
using namespace ascs::ext;
using namespace ascs::ext::tcp;

#define QUIT_COMMAND	"quit"
#define RESTART_COMMAND	"restart"
#define STATUS			"status"
#define STATISTIC		"statistic"
#define LIST_ALL_CLIENT	"list all client"
#define INCREASE_THREAD	"increase thread"
#define DECREASE_THREAD	"decrease thread"

//demonstrate how to use custom packer
//under the default behavior, each tcp::socket has their own packer, and cause memory waste
//at here, we make each echo_socket use the same global packer for memory saving
//notice: do not do this for unpacker, because unpacker has member variables and can't share each other
auto global_packer(std::make_shared<ASCS_DEFAULT_PACKER>());

//demonstrate how to control the type of tcp::server_socket_base::server from template parameter
class i_echo_server : public i_server
{
public:
	virtual void test() = 0;
};

class echo_socket : public server_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER, i_echo_server>
{
public:
	echo_socket(i_echo_server& server_) : server_socket_base(server_)
	{
		packer(global_packer);

#if 3 == PACKER_UNPACKER_TYPE
		std::dynamic_pointer_cast<ASCS_DEFAULT_UNPACKER>(unpacker())->prefix_suffix("begin", "end");
#endif
	}

public:
	//because we use objects pool(REUSE_OBJECT been defined), so, strictly speaking, this virtual
	//function must be rewrote, but we don't have member variables to initialize but invoke father's
	//reset() directly, so, it can be omitted, but we keep it for possibly future using
	virtual void reset() {server_socket_base::reset();}

protected:
	virtual void on_recv_error(const asio::error_code& ec)
	{
		//the type of tcp::server_socket_base::server now can be controlled by derived class(echo_socket),
		//which is actually i_echo_server, so, we can invoke i_echo_server::test virtual function.
		get_server().test();
		server_socket_base::on_recv_error(ec);
	}

	//msg handling: send the original msg back(echo server)
#ifdef ASCS_SYNC_DISPATCH //do not open this feature
	//do not hold msg_can for further using, return from on_msg as quickly as possible
	//access msg_can freely within this callback, it's always thread safe.
	virtual size_t on_msg(list<out_msg_type>& msg_can)
	{
		if (!is_send_buffer_available())
			return 0;
		//here if we cannot handle all messages in msg_can, do not use sync message dispatching except we can bear message disordering,
		//this is because on_msg_handle can be invoked concurrently with the next on_msg (new messages arrived) and then disorder messages.
		//and do not try to handle all messages here (just for echo_server's business logic) because:
		//1. we can not use safe_send_msg as i said many times, we should not block service threads.
		//2. if we use true can_overflow to call send_msg, then buffer usage will be out of control, we should not take this risk.

		//following statement can avoid one memory replication if the type of out_msg_type and in_msg_type are identical.
		ascs::do_something_to_all(msg_can, [this](out_msg_type& msg) {this->send_msg(std::move(msg), true);});
		auto re = msg_can.size();
		msg_can.clear();

		return re;
	}
#endif

#ifdef ASCS_DISPATCH_BATCH_MSG
	//do not hold msg_can for further using, access msg_can and return from on_msg_handle as quickly as possible
	//can only access msg_can via functions that marked as 'thread safe', if you used non-lock queue, its your responsibility to guarantee
	// that new messages will not come until we returned from this callback (for example, pingpong test).
	virtual size_t on_msg_handle(out_queue_type& msg_can)
	{
		if (!is_send_buffer_available())
			return 0;

		out_container_type tmp_can;
		msg_can.move_items_out(tmp_can, 10); //don't be too greedy, here is in a service thread, we should not block this thread for a long time

		//following statement can avoid one memory replication if the type of out_msg_type and in_msg_type are identical.
		ascs::do_something_to_all(tmp_can, [this](out_msg_type& msg) {this->send_msg(std::move(msg), true);});
		return tmp_can.size();
	}
#else
	//following statement can avoid one memory replication if the type of out_msg_type and in_msg_type are identical.
	virtual bool on_msg_handle(out_msg_type& msg) {return send_msg(std::move(msg));}
#endif
	//msg handling end
};

class echo_server : public server_base<echo_socket, object_pool<echo_socket>, i_echo_server>
{
public:
	echo_server(service_pump& service_pump_) : server_base(service_pump_) {}

protected:
	//from i_echo_server, pure virtual function, we must implement it.
	virtual void test() {/*puts("in echo_server::test()");*/}
};

#if ASCS_HEARTBEAT_INTERVAL > 0
typedef server_socket_base<packer, unpacker> normal_socket;
#else
//demonstrate how to open heartbeat function without defining macro ASCS_HEARTBEAT_INTERVAL
class normal_socket : public server_socket_base<packer, unpacker>
{
public:
	normal_socket(i_server& server_) : server_socket_base(server_) {}

protected:
	//demo client needs heartbeat (macro ASCS_HEARTBEAT_INTERVAL been defined), please note that the interval (here is 5) must be equal to
	//macro ASCS_HEARTBEAT_INTERVAL defined in demo client, and macro ASCS_HEARTBEAT_MAX_ABSENCE must has the same value as demo client's.
	virtual void on_connect() {start_heartbeat(5);}
};
#endif

//demonstrate how to accept just one client at server endpoint
class normal_server : public server_base<normal_socket>
{
public:
	normal_server(service_pump& service_pump_) : server_base(service_pump_) {}

protected:
	virtual int async_accept_num() {return 1;} //this make on_accept to be called in single thread, because stop_listen() is not thread safe
	virtual bool on_accept(object_ctype& socket_ptr) {stop_listen(); return true;}
};

class short_connection : public server_socket_base<packer, unpacker>
{
public:
	short_connection(i_server& server_) : server_socket_base(server_) {}

protected:
	//msg handling
#ifdef ASCS_SYNC_DISPATCH
	//do not hold msg_can for further using, return from on_msg as quickly as possible
	//access msg_can freely within this callback, it's always thread safe.
	virtual size_t on_msg(list<out_msg_type>& msg_can) {auto re = server_socket_base::on_msg(msg_can); force_shutdown(); return re;}
#endif

#ifdef ASCS_DISPATCH_BATCH_MSG
	//do not hold msg_can for further using, access msg_can and return from on_msg_handle as quickly as possible
	//can only access msg_can via functions that marked as 'thread safe', if you used non-lock queue, its your responsibility to guarantee
	// that new messages will not come until we returned from this callback (for example, pingpong test).
	virtual size_t on_msg_handle(out_queue_type& msg_can) {auto re = server_socket_base::on_msg_handle(msg_can); force_shutdown(); return re;}
#else
	virtual bool on_msg_handle(out_msg_type& msg) {auto re = server_socket_base::on_msg_handle(msg); force_shutdown(); return re;}
#endif
	//msg handling end
};

int main(int argc, const char* argv[])
{
	printf("usage: %s [<service thread number=1> [<port=%d> [ip=0.0.0.0]]]\n", argv[0], ASCS_SERVER_PORT);
	puts("normal server's port will be 100 larger.");
	if (argc >= 2 && (0 == strcmp(argv[1], "--help") || 0 == strcmp(argv[1], "-h")))
		return 0;
	else
		puts("type " QUIT_COMMAND " to end.");

	service_pump sp;
	//because of normal_socket, this server cannot support fixed_length_packer/fixed_length_unpacker and prefix_suffix_packer/prefix_suffix_unpacker,
	//the reason is these packer and unpacker need additional initializations that normal_socket not implemented, see echo_socket's constructor for more details.
	normal_server normal_server_(sp);
	echo_server echo_server_(sp); //echo server
	server_base<short_connection> short_server(sp);

	unsigned short port = ASCS_SERVER_PORT;
	std::string ip;
	if (argc > 2)
		port = (unsigned short) atoi(argv[2]);
	if (argc > 3)
		ip = argv[3];

	normal_server_.set_server_addr(port + 100, ip);
	short_server.set_server_addr(port + 200, ip);
	echo_server_.set_server_addr(port, ip);

	auto thread_num = 1;
	if (argc > 1)
		thread_num = std::min(16, std::max(thread_num, atoi(argv[1])));

#if 3 == PACKER_UNPACKER_TYPE
	global_packer->prefix_suffix("begin", "end");
#endif

	sp.start_service(thread_num);
	while(sp.is_running())
	{
		std::string str;
		std::getline(std::cin, str);
		if (str.empty())
			;
		else if (QUIT_COMMAND == str)
			sp.stop_service();
		else if (RESTART_COMMAND == str)
		{
			sp.stop_service();
			sp.start_service(thread_num);
		}
		else if (STATISTIC == str)
		{
			printf("normal server, link #: " ASCS_SF ", invalid links: " ASCS_SF "\n", normal_server_.size(), normal_server_.invalid_object_size());
			printf("echo server, link #: " ASCS_SF ", invalid links: " ASCS_SF "\n\n", echo_server_.size(), echo_server_.invalid_object_size());
			puts(echo_server_.get_statistic().to_string().data());
		}
		else if (STATUS == str)
		{
			normal_server_.list_all_status();
			echo_server_.list_all_status();
		}
		else if (LIST_ALL_CLIENT == str)
		{
			puts("clients from normal server:");
			normal_server_.list_all_object();
			puts("clients from echo server:");
			echo_server_.list_all_object();
		}
		else if (INCREASE_THREAD == str)
			sp.add_service_thread(1);
		else if (DECREASE_THREAD == str)
			sp.del_service_thread(1);
		else
		{
//			/*
			//broadcast series functions call pack_msg for each client respectively, because clients may used different protocols(so different type of packers, of course)
			normal_server_.broadcast_msg(str.data(), str.size() + 1, false);
			//send \0 character too, because demo client used basic_buffer as its msg type, it will not append \0 character automatically as std::string does,
			//so need \0 character when printing it.
//			*/
			/*
			//if all clients used the same protocol, we can pack msg one time, and send it repeatedly like this:
			packer p;
			auto msg = p.pack_msg(str.data(), str.size() + 1);
			//send \0 character too, because demo client used basic_buffer as its msg type, it will not append \0 character automatically as std::string does,
			//so need \0 character when printing it.
			if (!msg.empty())
				normal_server_.do_something_to_all([&msg](server_base<normal_socket>::object_ctype& item) {item->direct_send_msg(msg);});
			*/
			/*
			//if demo client is using stream_unpacker
			normal_server_.do_something_to_all([&str](server_base<normal_socket>::object_ctype& item) {item->direct_send_msg(str);});
			*/
		}
	}

	return 0;
}
