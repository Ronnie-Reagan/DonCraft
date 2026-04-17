#include "app/application.hpp"

#include "core/log.hpp"

#include <exception>

int main()
{
    try
    {
        df::Application application;
        return application.Run();
    }
    catch (const std::exception& error)
    {
        df::LogError("Fatal startup failure: ", error.what());
        return 1;
    }
}
