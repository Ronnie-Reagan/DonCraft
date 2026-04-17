#include "app/server_application.hpp"

#include "core/log.hpp"

#include <exception>

int main(int argc, char** argv)
{
    try
    {
        df::ServerApplication application;
        return application.Run(argc, argv);
    }
    catch (const std::exception& error)
    {
        df::LogError("Fatal dedicated-server startup failure: ", error.what());
        return 1;
    }
}
