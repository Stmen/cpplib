#pragma once

#include "asio.hpp"
#include "concurrentqueue.hpp"
#include "thread_pool.hpp"
#include "trivial_copyable_tuple.hpp"
#include "param.hpp"

#include <functional>
#include <iostream>
#include <atomic>
#include <memory>

#include <cassert>

namespace oy
{

namespace rpc
{
class Client
{
    enum class Vacancy : uint8_t { AVAILABLE, OCCUPIED };

    using deadline_timer_t = boost::asio::basic_waitable_timer<std::chrono::steady_clock>;
    using callback_t = std::function<void(const boost::system::system_error&, nlohmann::json j)>;
    using tuple_t = augs::trivially_copyable_tuple<Vacancy, unsigned long long, callback_t*, deadline_timer_t*>;
    using second_t = std::chrono::seconds;
    template <typename ... Args> decltype(auto) make_tuple_t(Args... args){ return augs::trivially_copyable_tuple<Args...>(args...); }

    template <typename T>
    class DecAtomicOnDistruct
    {
    public:
        DecAtomicOnDistruct(std::atomic<T>& a_):a(a_){}
        ~DecAtomicOnDistruct(){--a;}
    private:
        std::atomic<T> & a;
    };
public:                                                           //                                on error
    enum class Status { CONNECTING, READY, IDLE, SHUTTING_DOWN }; // IDLE -> CONNECTING -> READY -------------> SHUTTING_DOWN -> IDLE;
private:
    std::atomic<Status> status;
    static constexpr int HANDLE_POOL_SIZE = 1000;

    class ReturnHelper
    {
        friend class Client;
    public:
        template <typename T>
        T as() {
            try {
                return j.at("result").get<T>();
            } catch (const nlohmann::json::exception& e) {
                std::cout << "json is " << j.dump() << std::endl;
                return T();
            }
        }
    private:
        template<typename JSON>
        ReturnHelper(JSON&& j_):j(std::forward<JSON>(j_)){}
        nlohmann::json j;
    };
public:
    Client():
        status(Status::IDLE),
        io_service_work(std::make_unique<boost::asio::io_service::work>(io_service)),
        socket(io_service),
        pool_vacancy(HANDLE_POOL_SIZE),
        has_pending_callback(0)
    {
        // TODO: here
        async_run(4);
    }

    Client& connect(std::string ip_, unsigned short port_)
    {
        ip = ip_;
        port = port_;
        status = Status::CONNECTING;
        socket.connect(ip,port);
        status = Status::READY;
        has_pending_callback++;
        boost::asio::async_read(*socket.get_sock_ptr(), boost::asio::buffer(head_buffer),
                                [this](const boost::system::system_error& e, size_t sz) { this->boost_callback_1(e,sz); });
        return *this;
    }
    ~Client() {
        if(socket)
        {
            socket.cancel();
            socket.shutdown();
            while(has_pending_callback)
            {
                // maybe semaphore
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        io_service_work.reset(nullptr);
    }
    auto& get_status() const { return status; }
    void async_run(unsigned int n) {
        while(n--)
            async_io_service_run_thread.emplace_back(std::async(std::launch::async,[this](){this->io_service.run();}));
    }
    template <typename Duration> Client& set_connection_timeout(Duration&& t) { socket.set_connection_timeout(t); return *this; }
    template <typename Duration> Client& set_read_timeout(Duration&& t) { socket.set_read_timeout(t); return *this; }
public:
    template <typename ...Args>
    auto call(std::string name, Args&& ... args) {
        return call_with_timeout(static_cast<second_t*>(nullptr), name, std::forward<Args>(args)...);
    }
    template <typename Duration, typename ...Args>
    auto call_with_timeout(Duration d, std::string name, Args&& ... args) {
        return call_with_timeout(&d, name, std::forward<Args>(args)...);
    }

    // return true if function is registered; registered functions are guaranteed to be called-back.
    template <typename Functor, typename ...Args>
    bool callback(std::string name, Functor f, Args... args) {
        return callback_with_timeout(static_cast<second_t*>(nullptr), std::forward<Functor>(f), std::move(name), std::forward<Args>(args)...);
    }
    template <typename Duration, typename Functor, typename ...Args>
    bool callback_with_timeout(Duration d, Functor&& f, std::string name, Args&& ... args) {
        return callback_with_timeout(&d, f, name, std::forward<Args>(args)...);
    }
private:
    template <typename Duration, typename ...Args>
    auto call_with_timeout(Duration *d, std::string name, Args&& ... args)
    {
        Semaphore sem;
        nlohmann::json ret;
        if(!callback_with_timeout(d, [&sem, &ret](const boost::system::system_error& e, nlohmann::json j){
                    if(e.code())
                        j["boost error"] = e.what();
                    ret = std::move(j);
                    sem.notify();
                }, name, std::forward<Args>(args)...))
            return nlohmann::json({{"error", "call back failed"}});
        else
        {
            sem.wait();
        }
        return ret;
    }

    template <typename Duration, typename Functor, typename ...Args>
    bool callback_with_timeout(Duration* d, Functor&& f, std::string name, Args&& ... args) {
    // return true if function is registered; registered functions are guaranteed to be called-back.
    // template <typename Functor, typename ...Args>
    // bool callback(std::string name, Functor f, Args... args) {
        static_assert(std::is_same<typename func_traits<Functor>::args_type, std::tuple<boost::system::system_error, nlohmann::json> >::value,
                      "Callback function must of type void (boost::system::system_error, nlohmann::json)");
        static_assert(std::is_same<typename func_traits<Functor>::result_type, void>::value,
                      "Callback function must of type void (boost::system::system_error, nlohmann::json)");

        if (status.load() != Status::READY)
        {
            return false;
            //f(boost::system::system_error(boost::asio::error::bad_descriptor, "socket closed or not connected"), {});
            //return true;
        }
        auto previous = pool_vacancy.load();
        while(true) {
            if (previous == 0)
                return false;
            if (pool_vacancy.compare_exchange_weak(previous, previous - 1))
                break;
        }

        auto local_id = id;
        std::unique_ptr<deadline_timer_t> timer;
        if(d)
            timer = std::make_unique<deadline_timer_t>(io_service);
        std::unique_ptr<callback_t> pf = std::make_unique<callback_t>(f);
        while(true) {
            local_id = id++;
            auto expected = handle_pool[local_id % HANDLE_POOL_SIZE].load();
            if (occupied(expected))
                continue;
            auto desired = make_tuple_t(Vacancy::OCCUPIED, local_id, pf.get(), timer.get());
            if (handle_pool[local_id % HANDLE_POOL_SIZE].compare_exchange_strong(expected, desired)) {
                if(d) {
                    timer->expires_from_now(*d);
                    timer->async_wait([local_id, this](const boost::system::system_error& e){
                            assert(e.code() == boost::system::errc::success || e.code() == boost::asio::error::operation_aborted);
                            if(e.code() == boost::system::errc::success) //successfully timed out -_-//
                                this->clean_up(local_id,
                                              boost::system::system_error(boost::asio::error::timed_out, "timeout"),
                                              {{"error", "timeout"}});});
                }
                pf.release();
                timer.release();
                break;
            }
        }
        if (status.load() != Status::READY)
        {
            clean_up(local_id, boost::system::system_error(boost::asio::error::bad_descriptor, "socket closed"), {{"error", "EBADF"}});
            return true;
        }

        std::vector<uint8_t> buf(sizeof(size_t) + MAGIC_HEADER_SIZE);
        std::copy(MAGIC_HEADER.begin(), MAGIC_HEADER.end(), buf.begin());
        auto old_size = buf.size();
        nlohmann::json::to_cbor({ {"func", name},
                                  {"id", local_id},
                                  {"args", serialize(std::forward<Args>(args)...)} }, buf);
        *(reinterpret_cast<size_t*>(&buf[MAGIC_HEADER_SIZE])) = buf.size()-old_size;
        boost::asio::async_write(*socket.get_sock_ptr(), boost::asio::buffer(buf),
                                 [this, local_id](const boost::system::system_error& e, size_t) {
                                     if(e.code())
                                         this->clean_up(local_id,e,{{"error", "write failed"}});
                                 });
        return true;
    }

    void boost_callback_1(const boost::system::system_error& e, size_t) {
        DecAtomicOnDistruct<decltype(has_pending_callback.load())> d(has_pending_callback);
        size_t sz = *reinterpret_cast<size_t*>(&head_buffer[MAGIC_HEADER_SIZE]);
        if(e.code())
            shutdown(e);
        else if (!std::equal(MAGIC_HEADER.begin(), MAGIC_HEADER.end(), head_buffer.begin()))
            shutdown(boost::system::system_error(boost::system::errc::make_error_code(boost::system::errc::protocol_error), "magic number wrong"));
        else if (sz > MAX_BODY_LENGTH)
            shutdown(boost::system::system_error(boost::system::errc::make_error_code(boost::system::errc::file_too_large), "request body too large"));
        else {
            read_buffer.resize(sz);
            has_pending_callback++;
            boost::asio::async_read(*socket.get_sock_ptr(), boost::asio::buffer(read_buffer),
                                    [this](const boost::system::system_error& e, size_t sz) { this->boost_callback_2(e, sz); });
        }
    }
    void boost_callback_2(const boost::system::system_error& e, size_t) {
        DecAtomicOnDistruct<decltype(has_pending_callback.load())> d(has_pending_callback);
        if(e.code())
            shutdown(e);
        else {
            unsigned long long local_id;
            nlohmann::json j;
            try {
                j = nlohmann::json::from_cbor(read_buffer);
                local_id = j["id"];
            } catch (const nlohmann::json::exception& e) {
                return shutdown(boost::system::system_error(boost::asio::error::fault, "server response no id"));
            }
            tuple_t expected = handle_pool[local_id % HANDLE_POOL_SIZE].load();
            if (!occupied(expected))
            {
                // maybe canceled;
            }
            else if (handle_pool[local_id % HANDLE_POOL_SIZE].compare_exchange_strong(expected, empty_tuple()))
                clean_tuple(expected, boost::system::system_error(boost::system::error_code()), j);

            has_pending_callback++;
            boost::asio::async_read(*socket.get_sock_ptr(), boost::asio::buffer(head_buffer),
                                    [this](const boost::system::system_error& e, size_t sz) { this->boost_callback_1(e,sz); });
        }
    }

    bool occupied(tuple_t& t)
    {
        return std::get<0>(t)==Vacancy::OCCUPIED;
    }
    void clean_tuple(tuple_t& t, const boost::system::system_error& e, nlohmann::json j)
    {
        std::unique_ptr<std::remove_reference_t<decltype(*std::get<2>(t))> > pf   (std::get<2>(t));
        std::unique_ptr<std::remove_reference_t<decltype(*std::get<3>(t))> > timer(std::get<3>(t));
        if (timer) timer->cancel(); // cancel first to prevent prolonged pf from causing timeout.
        if (pf) (*pf)(e, j);
        pool_vacancy++;
    }

    void shutdown(const boost::system::system_error& e)
    {
        while(true)
        {
            auto old_status = status.load();
            if (old_status == Status::IDLE || old_status == Status::SHUTTING_DOWN)
                return;
            if(status.compare_exchange_strong(old_status, Status::SHUTTING_DOWN))
            {
                auto local_id = id;
                while(pool_vacancy != HANDLE_POOL_SIZE) {
                    local_id = (local_id+HANDLE_POOL_SIZE-1) % HANDLE_POOL_SIZE;
                    auto expected = handle_pool[local_id].load();
                    if (!occupied(expected))
                        continue;
                    if (handle_pool[local_id].compare_exchange_strong(expected, empty_tuple()))
                        clean_tuple(expected, e, {{"error", "shutdown"}});
                }
                status = Status::IDLE;
            }
        }
    }

    // this function is called from multiple processes.
    // if handle_pool[local_id % HANDLE_POOL_SIZE] is actually local_id;
    //     clean it
    void clean_up(unsigned long long local_id, const boost::system::system_error& e, nlohmann::json j)
    {
        auto previous = handle_pool[local_id % HANDLE_POOL_SIZE].load();
        if (!occupied(previous) || std::get<1>(previous) != local_id)
            return;
        if (handle_pool[local_id % HANDLE_POOL_SIZE].compare_exchange_strong(previous, empty_tuple()))
            clean_tuple(previous, e, j);
    }
    tuple_t empty_tuple()
    {
        //auto static t = make_tuple_t(AVAILABLE, 0ull, nullptr, nullptr); // not working.
        auto static t = make_tuple_t(Vacancy::AVAILABLE, 0ull, static_cast<callback_t*>(nullptr), static_cast<deadline_timer_t*>(nullptr));
        return t;
    }
private:
    boost::asio::io_service io_service;
    std::unique_ptr<boost::asio::io_service::work> io_service_work;
    oy::Socket socket;
    std::string ip;
    unsigned short port;
    std::vector<std::future<void> > async_io_service_run_thread;
    unsigned long long id = 0;
    std::atomic<unsigned int> pool_vacancy; // roughly vacancy in handle_pool, may be less than real value.
    constexpr static std::array<uint8_t, 8> MAGIC_HEADER = {'p','h','i','l','b','a','b','a'};
    constexpr static int MAGIC_HEADER_SIZE = MAGIC_HEADER.size();
    constexpr static int MAX_BODY_LENGTH = 12345678;
    std::array<uint8_t, sizeof(size_t) + MAGIC_HEADER_SIZE> head_buffer;
    std::vector<uint8_t> read_buffer;
    std::array<std::atomic<tuple_t>, HANDLE_POOL_SIZE> handle_pool;
    std::atomic<int> has_pending_callback;
    std::chrono::milliseconds connection_timeout = std::chrono::milliseconds(1000);
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds(1000);
};

}

}