

//ifdef WIN32 //or 64?
/*
to compile on windows systems:
INCLUDE:
	wsock32
	ws2_32
	pthreadGC2
	libboost:
	system
	thread
	chrono
*/

#define BOOST_THREAD_PLATFORM_WIN32
#define BOOST_THREAD_USE_LIB
//endif

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <list> //vector?
#include <set>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include "p2p_message.hpp"

using boost::asio::ip::tcp;
typedef std::deque<p2p_message> p2p_message_queue;


class p2p_participant{
public:
  virtual ~p2p_participant() {}
  virtual void deliver(const p2p_message& msg) = 0;
};

typedef boost::shared_ptr<p2p_participant> p2p_participant_ptr;

class p2p_room{
public:
  void join(p2p_participant_ptr participant)
  {
    participants_.insert(participant);
    std::for_each(recent_msgs_.begin(), recent_msgs_.end(),
        boost::bind(&p2p_participant::deliver, participant, _1));
  }

  void leave(p2p_participant_ptr participant)
  {
    participants_.erase(participant);
  }

  void deliver(const p2p_message& msg)
  {
    recent_msgs_.push_back(msg);
    while (recent_msgs_.size() > max_recent_msgs)
      recent_msgs_.pop_front();

    std::for_each(participants_.begin(), participants_.end(),
        boost::bind(&p2p_participant::deliver, _1, boost::ref(msg)));
  }

private:
  std::set<p2p_participant_ptr> participants_;
  enum { max_recent_msgs = 100 };
  p2p_message_queue recent_msgs_;
};


class p2p_session : public p2p_participant, public boost::enable_shared_from_this<p2p_session>{
public:
	p2p_session(boost::asio::io_service& io_service, p2p_room& room)
		: socket_(io_service),
		  room_(room)
	{
	}

	tcp::socket& socket()
	{
		return socket_;
	}

	void start(){
		room_.join(shared_from_this());
        boost::asio::async_read(socket_,
                        boost::asio::buffer(read_msg_.data(), p2p_message::header_length),
                        boost::bind(&p2p_session::handle_read_header, shared_from_this(),
                                  boost::asio::placeholders::error));
	}

	void deliver(const p2p_message& msg){
		bool write_in_progress = !write_msgs_.empty();
		write_msgs_.push_back(msg);
		if (!write_in_progress)
		{
			boost::asio::async_write(socket_,
							    boost::asio::buffer(write_msgs_.front().data(),
							    write_msgs_.front().length()),
							    boost::bind(&p2p_session::handle_write, shared_from_this(),
									      boost::asio::placeholders::error));
		}
	}

	  void handle_read_header(const boost::system::error_code& error)
  {
    if (!error && read_msg_.decode_header())
    {
      boost::asio::async_read(socket_,
          boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
          boost::bind(&p2p_session::handle_read_body, shared_from_this(),
            boost::asio::placeholders::error));
    }
    else
    {
      room_.leave(shared_from_this());
    }
  }

  void handle_read_body(const boost::system::error_code& error)
  {
    if (!error)
    {
      room_.deliver(read_msg_);
      boost::asio::async_read(socket_,
          boost::asio::buffer(read_msg_.data(), p2p_message::header_length),
          boost::bind(&p2p_session::handle_read_header, shared_from_this(),
            boost::asio::placeholders::error));
    }
    else
    {
      room_.leave(shared_from_this());
    }
  }

  void handle_write(const boost::system::error_code& error)
  {
    if (!error)
    {
      write_msgs_.pop_front();
      if (!write_msgs_.empty())
      {
        boost::asio::async_write(socket_,
            boost::asio::buffer(write_msgs_.front().data(),
              write_msgs_.front().length()),
            boost::bind(&p2p_session::handle_write, shared_from_this(),
              boost::asio::placeholders::error));
      }
    }
    else
    {
      room_.leave(shared_from_this());
    }
  };

private:
  tcp::socket socket_;
  p2p_room& room_;
  p2p_message read_msg_;
  p2p_message_queue write_msgs_;
};

typedef boost::shared_ptr<p2p_session> p2p_session_ptr;

class p2p_server{
public:
    p2p_server(boost::asio::io_service& io_service,
               const tcp::endpoint& endpoint)
            : io_service_(io_service),
              acceptor_(io_service, endpoint)
   {
        p2p_session_ptr new_session(new p2p_session(io_service, room_));//room?
        acceptor_.async_accept(new_session->socket(),
                               boost::bind(&p2p_server::handle_accept, this, new_session,
                                           boost::asio::placeholders::error));
   }

   void handle_accept(p2p_session_ptr session,
                      const boost::system::error_code& error)
   {
        if(!error){
            session->start();
            p2p_session_ptr new_session(new p2p_session(io_service_, room_));
            acceptor_.async_accept(new_session->socket(),
                                   boost::bind(&p2p_server::handle_accept, this, new_session,
                                               boost::asio::placeholders::error));
        }
   }

private:
    boost::asio::io_service& io_service_;
    tcp::acceptor acceptor_;
    p2p_room room_;

};



class p2p_client{
public:
  p2p_client(boost::asio::io_service& io_service,
      tcp::resolver::iterator endpoint_iterator)
    : io_service_(io_service),
      socket_(io_service)
  {
    tcp::endpoint endpoint = *endpoint_iterator;
    socket_.async_connect(endpoint,
        boost::bind(&p2p_client::handle_connect, this,
          boost::asio::placeholders::error, ++endpoint_iterator));
  }

  void write(const p2p_message& msg)
  {
    io_service_.post(boost::bind(&p2p_client::do_write, this, msg));
  }

  void close()
  {
    io_service_.post(boost::bind(&p2p_client::do_close, this));
  }

private:

  void handle_connect(const boost::system::error_code& error,
      tcp::resolver::iterator endpoint_iterator)
  {
    if (!error)
    {
      boost::asio::async_read(socket_,
          boost::asio::buffer(read_msg_.data(), p2p_message::header_length),
          boost::bind(&p2p_client::handle_read_header, this,
            boost::asio::placeholders::error));
    }
    else if (endpoint_iterator != tcp::resolver::iterator())
    {
      socket_.close();
      tcp::endpoint endpoint = *endpoint_iterator;
      socket_.async_connect(endpoint,
          boost::bind(&p2p_client::handle_connect, this,
            boost::asio::placeholders::error, ++endpoint_iterator));
    }
  }

  void handle_read_header(const boost::system::error_code& error)
  {
    if (!error && read_msg_.decode_header())
    {
      boost::asio::async_read(socket_,
          boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
          boost::bind(&p2p_client::handle_read_body, this,
            boost::asio::placeholders::error));
    }
    else
    {
      do_close();
    }
  }

  void handle_read_body(const boost::system::error_code& error)
  {
    if (!error)
    {
      std::cout.write(read_msg_.body(), read_msg_.body_length());
      std::cout << "\n";
      boost::asio::async_read(socket_,
          boost::asio::buffer(read_msg_.data(), p2p_message::header_length),
          boost::bind(&p2p_client::handle_read_header, this,
            boost::asio::placeholders::error));
    }
    else
    {
      do_close();
    }
  }

  void do_write(p2p_message msg)
  {
    bool write_in_progress = !write_msgs_.empty();
    write_msgs_.push_back(msg);
    if (!write_in_progress)
    {
      boost::asio::async_write(socket_,
          boost::asio::buffer(write_msgs_.front().data(),
            write_msgs_.front().length()),
          boost::bind(&p2p_client::handle_write, this,
            boost::asio::placeholders::error));
    }
  }

  void handle_write(const boost::system::error_code& error)
  {
    if (!error)
    {
      write_msgs_.pop_front();
      if (!write_msgs_.empty())
      {
        boost::asio::async_write(socket_,
            boost::asio::buffer(write_msgs_.front().data(),
              write_msgs_.front().length()),
            boost::bind(&p2p_client::handle_write, this,
              boost::asio::placeholders::error));
      }
    }
    else
    {
      do_close();
    }
  }

  void do_close()
  {
    socket_.close();
  }

private:
  boost::asio::io_service& io_service_;
  tcp::socket socket_;
  p2p_message read_msg_;
  p2p_message_queue write_msgs_;
};


typedef boost::shared_ptr<p2p_client> p2p_client_ptr;
typedef std::list<p2p_client_ptr> p2p_client_list;

int main(int argc, char* argv[]){
    try{
        if(argc < 3){
            std::cerr << "Usage: basic_tcp_p2p <port> <host> [<host> ...]\n";
            return 1;
        }

        boost::asio::io_service io_service;


            tcp::endpoint endpoint(tcp::v4(), atoi(argv[1]));
        p2p_server my_server(io_service, endpoint);


        p2p_client_list my_clients;

            tcp::resolver resolver(io_service);
        for(int i=2;i<argc;i++){
            tcp::resolver::query query(argv[i], argv[1]);
            tcp::resolver::iterator iterator = resolver.resolve(query);

            p2p_client_ptr client(new p2p_client(io_service, iterator));
            my_clients.push_back(client);
        }

        //io_service.run();
        boost::thread t(boost::bind(&boost::asio::io_service::run, &io_service));

        char line[p2p_message::max_body_length +1];
        while(std::cin.getline(line, p2p_message::max_body_length + 1)){
            using namespace std;

            p2p_message msg;
            msg.body_length(std::strlen(line));
            std::memcpy(msg.body(), line, msg.body_length());
            msg.encode_header();

            std::list<p2p_client_ptr>::iterator it;
            for( it = my_clients.begin(); it != my_clients.end(); it++){
                (*it)->write(msg);
            }

        }
    }
    catch(std::exception& e){
        std::cerr << "EXCEPTION: " << e.what() << "\n";
    }

    return 0;
}
