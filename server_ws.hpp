#ifndef SERVER_WS_HPP
#define	SERVER_WS_HPP

#include "crypto.hpp"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/regex.hpp>

#include <unordered_map>
#include <thread>
#include <mutex>
#include <set>
#include <memory>
#include <atomic>

#include <iostream>

namespace SimpleWeb {
    template <class socket_type>
    class SocketServer;
        
    template <class socket_type>
    class SocketServerBase {
    public:
        class Connection {
            friend class SocketServerBase<socket_type>;
            friend class SocketServer<socket_type>;
            
        public:
            std::string method, path, http_version;

            std::unordered_map<std::string, std::string> header;

            boost::smatch path_match;
            
            std::string remote_endpoint_address;
            unsigned short remote_endpoint_port;
            
        private:
            //boost::asio::ssl::stream constructor needs move, until then we store socket as unique_ptr
            std::unique_ptr<socket_type> socket;
            
            boost::asio::strand strand;
            
            std::atomic<bool> closed;

            std::unique_ptr<boost::asio::deadline_timer> timer_idle;

            Connection(socket_type *socket): socket(socket), strand(socket->get_io_service()), closed(false) {}
            
            void read_remote_endpoint_data() {
                try {
                    remote_endpoint_address=socket->lowest_layer().remote_endpoint().address().to_string();
                    remote_endpoint_port=socket->lowest_layer().remote_endpoint().port();
                }
                catch(const std::exception& e) {
                    std::cerr << e.what() << std::endl;
                }
            }
        };
        
        class Message : public std::istream {
            friend class SocketServerBase<socket_type>;
            
        public:
            unsigned char fin_rsv_opcode;
            size_t size() {
                return length;
            }
            std::string string() {
                std::stringstream ss;
                ss << rdbuf();
                return ss.str();
            }
        private:
            Message(): std::istream(&streambuf) {}
            size_t length;
            boost::asio::streambuf streambuf;
        };
        
        class Endpoint {
            friend class SocketServerBase<socket_type>;
        private:
            std::set<std::shared_ptr<Connection> > connections;
            std::mutex connections_mutex;

        public:            
            std::function<void(std::shared_ptr<Connection>)> onopen;
            std::function<void(std::shared_ptr<Connection>, std::shared_ptr<Message>)> onmessage;
            std::function<void(std::shared_ptr<Connection>, const boost::system::error_code&)> onerror;
            std::function<void(std::shared_ptr<Connection>, int, const std::string&)> onclose;
            
            std::set<std::shared_ptr<Connection> > get_connections() {
                connections_mutex.lock();
                auto copy=connections;
                connections_mutex.unlock();
                return copy;
            }
        };
        
        class SendStream : public std::ostream {
            friend class SocketServerBase<socket_type>;
        private:
            boost::asio::streambuf streambuf;
            std::atomic<bool> sending;
        public:
            SendStream(): std::ostream(&streambuf), sending(false) {}
            size_t size() {
                return streambuf.size();
            }
        };
        
        std::map<std::string, Endpoint> endpoint;
        
    private:
        std::vector<std::pair<boost::regex, Endpoint*> > opt_endpoint;
        
    public:
        void start() {
            opt_endpoint.clear();
            for(auto& endp: endpoint) {
                opt_endpoint.emplace_back(boost::regex(endp.first), &endp.second);
            }
            
            accept();
            
            //If num_threads>1, start m_io_service.run() in (num_threads-1) threads for thread-pooling
            threads.clear();
            for(size_t c=1;c<num_threads;c++) {
                threads.emplace_back([this](){
                    asio_io_service.run();
                });
            }

            //Main thread
            asio_io_service.run();

            //Wait for the rest of the threads, if any, to finish as well
            for(auto& t: threads) {
                t.join();
            }
        }
        
        void stop() {
            asio_io_service.stop();
            
            for(auto& p: endpoint)
                p.second.connections.clear();
        }
        
        //fin_rsv_opcode: 129=one fragment, text, 130=one fragment, binary, 136=close connection
        //See http://tools.ietf.org/html/rfc6455#section-5.2 for more information
        void send(std::shared_ptr<Connection> connection, std::shared_ptr<SendStream> send_stream, 
                const std::function<void(const boost::system::error_code&)>& callback=nullptr, 
                unsigned char fin_rsv_opcode=129) const {
            if(fin_rsv_opcode!=136)
                timer_idle_reset(connection);
            
            std::shared_ptr<boost::asio::streambuf> buffer(new boost::asio::streambuf());
            std::ostream stream(buffer.get());

            size_t length=send_stream->size();

            stream.put(fin_rsv_opcode);
            //unmasked (first length byte<128)
            if(length>=126) {
                int num_bytes;
                if(length>0xffff) {
                    num_bytes=8;
                    stream.put(127);
                }
                else {
                    num_bytes=2;
                    stream.put(126);
                }
                
                for(int c=num_bytes-1;c>=0;c--) {
                    stream.put((length>>(8*c))%256);
                }
            }
            else
                stream.put(static_cast<unsigned char>(length));

            boost::asio::spawn(connection->strand, [this, connection, buffer, send_stream, callback](boost::asio::yield_context yield) {
                //Need to copy the callback-function in case its destroyed
                boost::system::error_code ec;
                boost::asio::async_write(*connection->socket, *buffer, yield[ec]);
                if(ec) {
                    if(callback)
                        callback(ec);
                    return;
                }

                if(send_stream->sending==true)
                    throw std::runtime_error("SendStream already in use! Only reuse a SendStream if you are sure a prior send operation using the stream is finished.");
                send_stream->sending=true;
                boost::asio::async_write(*connection->socket, send_stream->streambuf, yield[ec]);
                send_stream->sending=false;
                if(callback)
                    callback(ec);
            });
        }
        
        
        void send_close(std::shared_ptr<Connection> connection, int status, const std::string& reason="") const {
            //Send close only once (in case close is initiated by server)
            if(connection->closed.load()) {
                return;
            }
            connection->closed.store(true);
            
            auto send_stream=std::make_shared<SendStream>();
            
            send_stream->put(status>>8);
            send_stream->put(status%256);
            
            *send_stream << reason;

            //fin_rsv_opcode=136: message close
            send(connection, send_stream, [](const boost::system::error_code& /*ec*/){}, 136);
        }
        
        std::set<std::shared_ptr<Connection> > get_connections() {
            std::set<std::shared_ptr<Connection> > all_connections;
            for(auto& e: endpoint) {
                e.second.connections_mutex.lock();
                all_connections.insert(e.second.connections.begin(), e.second.connections.end());
                e.second.connections_mutex.unlock();
            }
            return all_connections;
        }
        
    protected:
        const std::string ws_magic_string="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                
        boost::asio::io_service asio_io_service;
        boost::asio::ip::tcp::endpoint asio_endpoint;
        boost::asio::ip::tcp::acceptor asio_acceptor;
        
        size_t num_threads;
        std::vector<std::thread> threads;
        
        size_t timeout_request;
        size_t timeout_idle;
        
        SocketServerBase(unsigned short port, size_t num_threads, size_t timeout_request, size_t timeout_idle) : 
                asio_endpoint(boost::asio::ip::tcp::v4(), port), asio_acceptor(asio_io_service, asio_endpoint),
                num_threads(num_threads), timeout_request(timeout_request), timeout_idle(timeout_idle) {}
        
        virtual void accept()=0;
        
        std::shared_ptr<boost::asio::deadline_timer> set_timeout_on_connection(std::shared_ptr<Connection> connection, size_t seconds) {
            std::shared_ptr<boost::asio::deadline_timer> timer(new boost::asio::deadline_timer(asio_io_service));
            timer->expires_from_now(boost::posix_time::seconds(static_cast<long>(seconds)));
            timer->async_wait([connection](const boost::system::error_code& ec){
                if(!ec) {
                    connection->socket->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both);
                    connection->socket->lowest_layer().close();
                }
            });
            return timer;
        }

        void read_handshake(std::shared_ptr<Connection> connection) {
            connection->read_remote_endpoint_data();
            
            //Create new read_buffer for async_read_until()
            //Shared_ptr is used to pass temporary objects to the asynchronous functions
            std::shared_ptr<boost::asio::streambuf> read_buffer(new boost::asio::streambuf);

            //Set timeout on the following boost::asio::async-read or write function
            std::shared_ptr<boost::asio::deadline_timer> timer;
            if(timeout_request>0)
                timer=set_timeout_on_connection(connection, timeout_request);
            
            boost::asio::async_read_until(*connection->socket, *read_buffer, "\r\n\r\n",
                    [this, connection, read_buffer, timer]
                    (const boost::system::error_code& ec, size_t /*bytes_transferred*/) {
                if(timeout_request>0)
                    timer->cancel();
                if(!ec) {
                    //Convert to istream to extract string-lines
                    std::istream stream(read_buffer.get());

                    parse_handshake(connection, stream);
                    
                    write_handshake(connection, read_buffer);
                }
            });
        }
        
        void parse_handshake(std::shared_ptr<Connection> connection, std::istream& stream) const {
            std::string line;
            getline(stream, line);
            size_t method_end=line.find(' ');
            size_t path_end=line.find(' ', method_end+1);
            if(method_end!=std::string::npos && path_end!=std::string::npos) {
                connection->method=line.substr(0, method_end);
                connection->path=line.substr(method_end+1, path_end-method_end-1);
                connection->http_version=line.substr(path_end+6, line.size()-path_end-7);

                getline(stream, line);
                size_t param_end=line.find(':');
                while(param_end!=std::string::npos) {                
                    size_t value_start=param_end+1;
                    if(line[value_start]==' ')
                        value_start++;

                    connection->header[line.substr(0, param_end)]=line.substr(value_start, line.size()-value_start-1);

                    getline(stream, line);
                    param_end=line.find(':');
                }
            }
        }
        
        void write_handshake(std::shared_ptr<Connection> connection, std::shared_ptr<boost::asio::streambuf> read_buffer) {
            //Find path- and method-match, and generate response
            for(auto& endp: opt_endpoint) {
                boost::smatch path_match;
                if(boost::regex_match(connection->path, path_match, endp.first)) {
                    std::shared_ptr<boost::asio::streambuf> write_buffer(new boost::asio::streambuf);
                    std::ostream handshake(write_buffer.get());

                    if(generate_handshake(connection, handshake)) {
                        connection->path_match=std::move(path_match);
                        //Capture write_buffer in lambda so it is not destroyed before async_write is finished
                        boost::asio::async_write(*connection->socket, *write_buffer, 
                                [this, connection, write_buffer, read_buffer, &endp]
                                (const boost::system::error_code& ec, size_t /*bytes_transferred*/) {
                            if(!ec) {
                                connection_open(connection, *endp.second);
                                read_message(connection, read_buffer, *endp.second);
                            }
                            else
                                connection_error(connection, *endp.second, ec);
                        });
                    }
                    return;
                }
            }
        }
        
        bool generate_handshake(std::shared_ptr<Connection> connection, std::ostream& handshake) const {
            if(connection->header.count("Sec-WebSocket-Key")==0)
                return 0;
            
            auto sha1=Crypto::SHA1(connection->header["Sec-WebSocket-Key"]+ws_magic_string);

            handshake << "HTTP/1.1 101 Web Socket Protocol Handshake\r\n";
            handshake << "Upgrade: websocket\r\n";
            handshake << "Connection: Upgrade\r\n";
            handshake << "Sec-WebSocket-Accept: " << Crypto::Base64::encode(sha1) << "\r\n";
            handshake << "\r\n";
            
            return 1;
        }
        
        void read_message(std::shared_ptr<Connection> connection, 
                std::shared_ptr<boost::asio::streambuf> read_buffer, Endpoint& endpoint) const {
            boost::asio::async_read(*connection->socket, *read_buffer, boost::asio::transfer_exactly(2),
                    [this, connection, read_buffer, &endpoint]
                    (const boost::system::error_code& ec, size_t /*bytes_transferred*/) {
                if(!ec) {
                    std::istream stream(read_buffer.get());

                    std::vector<unsigned char> first_bytes;
                    first_bytes.resize(2);
                    stream.read((char*)&first_bytes[0], 2);
                    
                    unsigned char fin_rsv_opcode=first_bytes[0];
                    
                    //Close connection if unmasked message from client (protocol error)
                    if(first_bytes[1]<128) {
                        const std::string reason="message from client not masked";
                        send_close(connection, 1002, reason);
                        connection_close(connection, endpoint, 1002, reason);
                        return;
                    }
                    
                    size_t length=(first_bytes[1]&127);

                    if(length==126) {
                        //2 next bytes is the size of content
                        boost::asio::async_read(*connection->socket, *read_buffer, boost::asio::transfer_exactly(2),
                                [this, connection, read_buffer, &endpoint, fin_rsv_opcode]
                                (const boost::system::error_code& ec, size_t /*bytes_transferred*/) {
                            if(!ec) {
                                std::istream stream(read_buffer.get());
                                
                                std::vector<unsigned char> length_bytes;
                                length_bytes.resize(2);
                                stream.read((char*)&length_bytes[0], 2);
                                
                                size_t length=0;
                                int num_bytes=2;
                                for(int c=0;c<num_bytes;c++)
                                    length+=length_bytes[c]<<(8*(num_bytes-1-c));
                                
                                read_message_content(connection, read_buffer, length, endpoint, fin_rsv_opcode);
                            }
                            else
                                connection_error(connection, endpoint, ec);
                        });
                    }
                    else if(length==127) {
                        //8 next bytes is the size of content
                        boost::asio::async_read(*connection->socket, *read_buffer, boost::asio::transfer_exactly(8),
                                [this, connection, read_buffer, &endpoint, fin_rsv_opcode]
                                (const boost::system::error_code& ec, size_t /*bytes_transferred*/) {
                            if(!ec) {
                                std::istream stream(read_buffer.get());
                                
                                std::vector<unsigned char> length_bytes;
                                length_bytes.resize(8);
                                stream.read((char*)&length_bytes[0], 8);
                                
                                size_t length=0;
                                int num_bytes=8;
                                for(int c=0;c<num_bytes;c++)
                                    length+=length_bytes[c]<<(8*(num_bytes-1-c));

                                read_message_content(connection, read_buffer, length, endpoint, fin_rsv_opcode);
                            }
                            else
                                connection_error(connection, endpoint, ec);
                        });
                    }
                    else
                        read_message_content(connection, read_buffer, length, endpoint, fin_rsv_opcode);
                }
                else
                    connection_error(connection, endpoint, ec);
            });
        }
        
        void read_message_content(std::shared_ptr<Connection> connection, 
                std::shared_ptr<boost::asio::streambuf> read_buffer, 
                size_t length, Endpoint& endpoint, unsigned char fin_rsv_opcode) const {
            boost::asio::async_read(*connection->socket, *read_buffer, boost::asio::transfer_exactly(4+length),
                    [this, connection, read_buffer, length, &endpoint, fin_rsv_opcode]
                    (const boost::system::error_code& ec, size_t /*bytes_transferred*/) {
                if(!ec) {
                    std::istream raw_message_data(read_buffer.get());

                    //Read mask
                    std::vector<unsigned char> mask;
                    mask.resize(4);
                    raw_message_data.read((char*)&mask[0], 4);
                    
                    std::shared_ptr<Message> message(new Message());
                    message->length=length;
                    message->fin_rsv_opcode=fin_rsv_opcode;
                    
                    std::ostream message_data_out_stream(&message->streambuf);
                    for(size_t c=0;c<length;c++) {
                        message_data_out_stream.put(raw_message_data.get()^mask[c%4]);
                    }
                    
                    //If connection close
                    if((fin_rsv_opcode&0x0f)==8) {
                        int status=0;
                        if(length>=2) {
                            unsigned char byte1=message->get();
                            unsigned char byte2=message->get();
                            status=(byte1<<8)+byte2;
                        }
                        
                        auto reason=message->string();
                        send_close(connection, status, reason);
                        connection_close(connection, endpoint, status, reason);
                        return;
                    }
                    else {
                        //If ping
                        if((fin_rsv_opcode&0x0f)==9) {
                            //send pong
                            auto empty_send_stream=std::make_shared<SendStream>();
                            send(connection, empty_send_stream, nullptr, fin_rsv_opcode+1);
                        }
                        else if(endpoint.onmessage) {
                            timer_idle_reset(connection);
                            endpoint.onmessage(connection, message);
                        }
    
                        //Next message
                        read_message(connection, read_buffer, endpoint);
                    }
                }
                else
                    connection_error(connection, endpoint, ec);
            });
        }
        
        void connection_open(std::shared_ptr<Connection> connection, Endpoint& endpoint) {
            timer_idle_init(connection);
            
            endpoint.connections_mutex.lock();
            endpoint.connections.insert(connection);
            endpoint.connections_mutex.unlock();
            
            if(endpoint.onopen)
                endpoint.onopen(connection);
        }
        
        void connection_close(std::shared_ptr<Connection> connection, Endpoint& endpoint, int status, const std::string& reason) const {
            timer_idle_cancel(connection);
            
            endpoint.connections_mutex.lock();
            endpoint.connections.erase(connection);
            endpoint.connections_mutex.unlock();    
            
            if(endpoint.onclose)
                endpoint.onclose(connection, status, reason);
        }
        
        void connection_error(std::shared_ptr<Connection> connection, Endpoint& endpoint, const boost::system::error_code& ec) const {
            timer_idle_cancel(connection);
            
            endpoint.connections_mutex.lock();
            endpoint.connections.erase(connection);
            endpoint.connections_mutex.unlock();
            
            if(endpoint.onerror) {
                boost::system::error_code ec_tmp=ec;
                endpoint.onerror(connection, ec_tmp);
            }
        }
        
        void timer_idle_init(std::shared_ptr<Connection> connection) {
            if(timeout_idle>0) {
                connection->timer_idle=std::unique_ptr<boost::asio::deadline_timer>(new boost::asio::deadline_timer(asio_io_service));
                connection->timer_idle->expires_from_now(boost::posix_time::seconds(static_cast<unsigned long>(timeout_idle)));
                timer_idle_expired_function(connection);
            }
        }
        void timer_idle_reset(std::shared_ptr<Connection> connection) const {
            if(timeout_idle>0 && connection->timer_idle->expires_from_now(boost::posix_time::seconds(static_cast<unsigned long>(timeout_idle)))>0) {
                timer_idle_expired_function(connection);
            }
        }
        void timer_idle_cancel(std::shared_ptr<Connection> connection) const {
            if(timeout_idle>0)
                connection->timer_idle->cancel();
        }
        
        void timer_idle_expired_function(std::shared_ptr<Connection> connection) const {
            connection->timer_idle->async_wait([this, connection](const boost::system::error_code& ec){
                if(!ec) {
                    //1000=normal closure
                    send_close(connection, 1000, "idle timeout");
                }
            });
        }
    };
    
    template<class socket_type>
    class SocketServer : public SocketServerBase<socket_type> {};
    
    typedef boost::asio::ip::tcp::socket WS;
    
    template<>
    class SocketServer<WS> : public SocketServerBase<WS> {
    public:
        SocketServer(unsigned short port, size_t num_threads=1, size_t timeout_request=5, size_t timeout_idle=0) : 
                SocketServerBase<WS>::SocketServerBase(port, num_threads, timeout_request, timeout_idle) {};
        
    private:
        void accept() {
            //Create new socket for this connection (stored in Connection::socket)
            //Shared_ptr is used to pass temporary objects to the asynchronous functions
            std::shared_ptr<Connection> connection(new Connection(new WS(asio_io_service)));
            
            asio_acceptor.async_accept(*connection->socket, [this, connection](const boost::system::error_code& ec) {
                //Immediately start accepting a new connection
                accept();
                if(!ec) {
                    read_handshake(connection);
                }
            });
        }
    };
}
#endif	/* SERVER_WS_HPP */
