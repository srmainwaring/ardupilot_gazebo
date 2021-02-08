// Extracting function pointers from functions created with std::bind

#include <exception>
#include <functional>
#include <iostream>
#include <string>


// create a subscriber from a pointer to a function
void subscribe(void(*callback)(const std::string& msg))
{
    callback("function created subscriber");
}

// create a subscriber from a pointer to a member function
template<typename T>
void subscribe(void(T::*callback)(const std::string& msg), T* obj)
{
    (obj->*callback)("member function created subscriber");
}

// callback with single argument
void on_message(const std::string& msg)
{
    std::cout << "on_message recv: " << msg << "\n";
}

// callback with additional argument
void on_message2(const std::string& msg, int index)
{
    std::cout << "on_message2 recv: " << msg << ", index: " << index << "\n";
}

// class containing call back member functions
class MessageHandler
{
public:
    void on_message(const std::string& msg)
    {
        std::cout << "on_message recv: " << msg << "\n";
    }

    void on_message2(const std::string& msg, int index)
    {
        std::cout << "on_message2 recv: " << msg << ", index: " << index << "\n";
    }
};

// A wrapper class to enable subscribe() to register the std::function callback 
template<typename T>
class MessageHandlerWrapper
{
public:
    T callback_;
    MessageHandlerWrapper(const T& callback) : callback_(callback) {}

    inline void on_message(const std::string& msg)
    {
        callback_(msg);
    }
};

// explicit types
void foo1()
{
    subscribe(&on_message);
}

// pointer to function
void foo2()
{
    void (*callback)(const std::string& msg) = &on_message;
    subscribe(callback);
}

// pointer to function using auto
void foo3()
{
    auto callback = &on_message;
    subscribe(callback);
}

// pointer to member function
void foo4()
{
    MessageHandler msg_handler;
    void (MessageHandler::*callback)(const std::string& msg) = &MessageHandler::on_message;
    subscribe(callback, &msg_handler);
}

// std::function extracting target to auto
void foo14()
{
    typedef std::function<void(const std::string& msg)> callback_t;
    callback_t fn = on_message;

    MessageHandlerWrapper<callback_t> msg_handler_wrapper(fn);
    auto callback = &MessageHandlerWrapper<callback_t>::on_message;

    subscribe(callback, &msg_handler_wrapper);
}

// std::function with std::bind and placeholder extracting target to auto
void foo15()
{
    typedef std::function<void(const std::string& msg)> callback_t;
    callback_t fn = std::bind(on_message, std::placeholders::_1);

    MessageHandlerWrapper<callback_t> msg_handler_wrapper(fn);
    auto callback = &MessageHandlerWrapper<callback_t>::on_message;

    subscribe(callback, &msg_handler_wrapper);
}

// std::function with std::bind and placeholder extracting target to auto
void foo16()
{
    typedef std::function<void(const std::string& msg)> callback_t;
    callback_t fn = std::bind(on_message2, std::placeholders::_1, 10);

    MessageHandlerWrapper<callback_t> msg_handler_wrapper(fn);
    auto callback = &MessageHandlerWrapper<callback_t>::on_message;

    subscribe(callback, &msg_handler_wrapper);
}

// std::function with std::bind and placeholder extracting target to auto
void foo17()
{
    MessageHandler msg_handler;

    typedef std::function<void(const std::string& msg)> callback_t;
    callback_t fn = std::bind(&MessageHandler::on_message, &msg_handler, std::placeholders::_1);

    MessageHandlerWrapper<callback_t> msg_handler_wrapper(fn);
    auto callback = &MessageHandlerWrapper<callback_t>::on_message;

    subscribe(callback, &msg_handler_wrapper);
}

// std::function with std::bind and placeholder extracting target to auto
void foo18()
{
    MessageHandler msg_handler;

    typedef std::function<void(const std::string& msg)> callback_t;
    callback_t fn = std::bind(&MessageHandler::on_message2, &msg_handler, std::placeholders::_1, 15);

    MessageHandlerWrapper<callback_t> msg_handler_wrapper(fn);
    auto callback = &MessageHandlerWrapper<callback_t>::on_message;

    subscribe(callback, &msg_handler_wrapper);
}

int main(int argc, const char* argv[])
{
    try
    {
        std::cout << "foo1\n";
        foo1();

        std::cout << "foo2\n";
        foo2();

        std::cout << "foo3\n";
        foo3();

        std::cout << "foo4\n";
        foo4();

        std::cout << "foo14\n";
        foo14();

        std::cout << "foo15\n";
        foo15();

        std::cout << "foo16\n";
        foo16();

        std::cout << "foo17\n";
        foo17();

        std::cout << "foo18\n";
        foo18();
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }

    return 0;
}
