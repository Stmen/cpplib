#include <string>
#include <vector>
#include <sstream>
#include <deque>
#include <fstream>
#include "thread_pool.hpp"
#include "rpc-client.hpp"

#include <chrono>

using namespace std::chrono_literals;

using namespace std;

string g_ip;
unsigned short g_port;

int g(const string & s, const string & ip, unsigned short port, const string & f)
{
    try {
        oy::rpc::Client client(ip, port);
        client.set_connection_timeout(std::chrono::milliseconds(50));
        client.set_read_timeout(std::chrono::milliseconds(1000));
        return client.call(f, s).as<int>();
    } catch (...)
    {
        return -1;
    }
}

void f(const string& s)
{
    auto r = g(s, g_ip, g_port, "count_words");
    stringstream ss;
    ss<<s;
    string x;
    unsigned int c=0;
    while(ss>>x)
        c++;
    if(false)
    {
        if(r==c)
            cout<<'.';
        else
            cout<<'\'';
        cout<< flush;
    }
}

int tmain(int argc, char* argv[] )
{
    g_ip = argv[1];
    g_port = stoi(argv[2]);
    oy::Distributor<string> d(f, std::stoi(argv[3]), std::stoi(argv[3]));
    string s;
    std::ifstream fin("all.txt");
    int count = (argc >= 5)?stoi(argv[4]):INT_MAX;
    while(getline(fin,s))
    {
        d(s);
        count--;
        if(count==0)
            break;
    }
    return 0;
}

using namespace std;

int main(int argc, char *argv[])
{
    g_ip = argv[1];
    g_port = stoi(argv[2]);

    return tmain(argc, argv);
    oy::rpc::Client client(g_ip, g_port);
    client.set_connection_timeout(std::chrono::milliseconds(50));
    client.set_read_timeout(std::chrono::milliseconds(1000));
    client.callback("count_words", [](const boost::system::system_error& e, nlohmann::json j){
            cout << "e is " << e.what() << endl;
            cout << "j is " << j.dump() << endl;
        }, "1 2 3 4 5");

    std::this_thread::sleep_for(1s);
    return 0;
}
